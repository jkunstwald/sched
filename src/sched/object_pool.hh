#pragma once

#include <atomic>
#include <thread>

#include "common/check.hh"
#include "common/config.hh"
#include "common/mem_callbacks.hh"

namespace sched
{
/// ObjectPool
/// holds up to 2^20 objects with ref counting and versioning
/// used internally by the Scheduler for tasks and counters, but can also
/// be used as a thread-safe object pool
template <class T>
struct ObjectPool
{
    const uint32_t kPosMask = 0x000FFFFF; // 20 bits
    const uint32_t kRefMask = kPosMask;   // 20 bits
    const uint32_t kVerMask = 0xFFF00000; // 12 bits
    const uint32_t kVerDisp = 20;

    ~ObjectPool();

    void init(uint32_t count, const MemCallbacks& mem = MemCallbacks());
    void reset();

    // only access objects you've previously referenced
    T& get(uint32_t hnd);

    // only access objects you've previously referenced
    const T& get(uint32_t hnd) const;

    // given a position inside the pool returns the handler and also current ref
    // count and current version number (only used for debugging)
    uint32_t info(size_t pos, uint32_t* count, uint32_t* ver) const;

    // max number of elements hold by the object pool
    size_t const& size() const { return count_; }

    // returns the handler of an object in the pool that can be used
    // it also increments in one the number of references (no need to call ref)
    uint32_t acquireAndRef();

    void unref(uint32_t hnd) const;

    // decrements the counter, if the object is no longer valid (last ref)
    // the given function will be executed with the element
    template <class F>
    void unref(uint32_t hnd, F f) const;

    // returns true if the given position was a valid object
    bool ref(uint32_t hnd) const;

    uint32_t refCount(uint32_t hnd) const;

private:
    void newElement(uint32_t pos) const;
    void deleteElement(uint32_t pos) const;
    struct D
    {
        mutable std::atomic<uint32_t> state = {0};
        uint32_t version = 0;
        T element;
#if SCHED_CACHE_LINE_SIZE
        // Avoid false sharing between threads
        char padding[SCHED_CACHE_LINE_SIZE];
#endif
    };
    D* data_ = nullptr;
    std::atomic<uint32_t> next_;
    size_t count_ = 0;
    MemCallbacks mem_;
};

//-- Object pool implementation ----------------------------------------------
template <class T>
inline ObjectPool<T>::~ObjectPool()
{
    reset();
}

template <class T>
void ObjectPool<T>::newElement(uint32_t pos) const
{
    new (&data_[pos].element) T;
}

template <class T>
void ObjectPool<T>::deleteElement(uint32_t pos) const
{
    data_[pos].element.~T();
}

template <class T>
inline void ObjectPool<T>::init(uint32_t count, const MemCallbacks& mem_cb)
{
    reset();
    mem_ = mem_cb;
    data_ = static_cast<D*>(mem_.alloc_fn(sizeof(D) * count));
    for (uint32_t i = 0; i < count; ++i)
    {
        data_[i].state = 0xFFFu << kVerDisp;
    }
    count_ = count;
    next_ = 0;
}

template <class T>
inline void ObjectPool<T>::reset()
{
    count_ = 0;
    next_ = 0;
    if (data_)
    {
        mem_.free_fn(data_);
        data_ = nullptr;
    }
}

// only access objects you've previously referenced
template <class T>
inline T& ObjectPool<T>::get(uint32_t hnd)
{
    uint32_t pos = hnd & kPosMask;
    SCHED_RUNTIME_ASSERT(pos < count_, "Invalid access to pos %u hnd:%zu", pos, count_);
    return data_[pos].element;
}

// only access objects you've previously referenced
template <class T>
inline const T& ObjectPool<T>::get(uint32_t hnd) const
{
    uint32_t pos = hnd & kPosMask;
    SCHED_RUNTIME_ASSERT(pos < count_, "Invalid access to pos %zu hnd:%zu", pos, count_);
    return data_[pos].element;
}

template <class T>
inline uint32_t ObjectPool<T>::info(size_t pos, uint32_t* count, uint32_t* ver) const
{
    SCHED_RUNTIME_ASSERT(pos < count_, "Invalid access to pos %zu hnd:%zu", pos, count_);
    uint32_t s = data_[pos].state.load();
    if (count)
        *count = (s & kRefMask);
    if (ver)
        *ver = (s & kVerMask) >> kVerDisp;
    return (s & kVerMask) | static_cast<uint32_t>(pos);
}

template <class T>
inline uint32_t ObjectPool<T>::acquireAndRef()
{
    uint32_t tries = 0;
    for (;;)
    {
        uint32_t pos = (next_.fetch_add(1) % count_);
        D& d = data_[pos];
        uint32_t version = (d.state.load() & kVerMask) >> kVerDisp;
        // note: avoid 0 as version
        uint32_t newver = (version + 1) & 0xFFF;
        if (newver == 0)
            newver = 1;
        // instead of using 1 as initial ref, we use 2, when we see 1
        // in the future we know the object must be freed, but it wont
        // be actually freed until it reaches 0
        uint32_t newvalue = (newver << kVerDisp) + 2;
        uint32_t expected = version << kVerDisp;
        if (d.state.compare_exchange_strong(expected, newvalue))
        {
            newElement(pos); //< initialize
            return (newver << kVerDisp) | (pos & kPosMask);
        }
        tries++;
        // TODO... make this, optional...
        SCHED_RUNTIME_ASSERT(tries < count_ * count_, "It was not possible to find a valid index after %u tries", tries);
    }
}

template <class T>
inline void ObjectPool<T>::unref(uint32_t hnd) const
{
    uint32_t pos = hnd & kPosMask;
    uint32_t ver = (hnd & kVerMask);
    D& d = data_[pos];
    for (;;)
    {
        uint32_t prev = d.state.load();
        uint32_t next = prev - 1;
        SCHED_RUNTIME_ASSERT((prev & kVerMask) == ver, "Invalid unref HND = %u(%u), Versions: %u vs %u", pos, hnd, prev & kVerMask, ver);
        SCHED_RUNTIME_ASSERT((prev & kRefMask) > 1, "Invalid unref HND = %u(%u), invalid ref count", pos, hnd);
        if (d.state.compare_exchange_strong(prev, next))
        {
            if ((next & kRefMask) == 1)
            {
                deleteElement(pos);
                d.state = 0;
            }
            return;
        }
    }
}

// decrements the counter, if the objet is no longer valid (las ref)
// the given function will be executed with the element
template <class T>
template <class F>
inline void ObjectPool<T>::unref(uint32_t hnd, F f) const
{
    uint32_t pos = hnd & kPosMask;
    uint32_t ver = (hnd & kVerMask);
    D& d = data_[pos];
    for (;;)
    {
        uint32_t prev = d.state.load();
        uint32_t next = prev - 1;
        SCHED_RUNTIME_ASSERT((prev & kVerMask) == ver, "Invalid unref HND = %u(%u), Versions: %u vs %u", pos, hnd, prev & kVerMask, ver);
        SCHED_RUNTIME_ASSERT((prev & kRefMask) > 1, "Invalid unref HND = %u(%u), invalid ref count", pos, hnd);
        if (d.state.compare_exchange_strong(prev, next))
        {
            if ((next & kRefMask) == 1)
            {
                f(d.element);
                deleteElement(pos);
                d.state = 0;
            }
            return;
        }
    }
}

template <class T>
inline bool ObjectPool<T>::ref(uint32_t hnd) const
{
    if (!hnd)
        return false;
    uint32_t pos = hnd & kPosMask;
    uint32_t ver = (hnd & kVerMask);
    D& d = data_[pos];
    for (;;)
    {
        uint32_t prev = d.state.load();
        uint32_t next_c = ((prev & kRefMask) + 1);
        if ((prev & kVerMask) != ver || next_c <= 2)
            return false;
        SCHED_RUNTIME_ASSERT(next_c == (next_c & kRefMask), "Too many references...");
        uint32_t next = (prev & kVerMask) | next_c;
        if (d.state.compare_exchange_strong(prev, next))
        {
            return true;
        }
    }
}

template <class T>
inline uint32_t ObjectPool<T>::refCount(uint32_t hnd) const
{
    if (!hnd)
        return 0;
    uint32_t pos = hnd & kPosMask;
    uint32_t ver = (hnd & kVerMask);
    D& d = data_[pos];
    uint32_t current = d.state.load();
    if ((current & kVerMask) != ver)
        return 0;
    return (current & kRefMask);
}

}
