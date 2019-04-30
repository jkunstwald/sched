#include <cstdlib>
#include <mutex>

#include <sched/scheduler.hh>
#include <sched/optional/spinlock.hh>

// Example 7 from px_sched:
// Multiple Readers, Single Writer pattern

namespace
{
size_t GLOBAL_amount_alloc = 0;
size_t GLOBAL_amount_dealloc = 0;

void* mem_check_alloc(size_t s)
{
    size_t* ptr = static_cast<size_t*>(malloc(sizeof(size_t) + s));
    GLOBAL_amount_alloc += s;
    *ptr = s;
    return ptr + 1;
}

void mem_check_free(void* raw_ptr)
{
    size_t* ptr = static_cast<size_t*>(raw_ptr);
    GLOBAL_amount_dealloc += ptr[-1];
    free(ptr - 1);
}

void mem_report()
{
    printf("Total memory allocated: %zu\n", GLOBAL_amount_alloc);
    printf("Total memory freed:     %zu\n", GLOBAL_amount_dealloc);
    if (GLOBAL_amount_alloc != GLOBAL_amount_dealloc)
        abort();
}
}


template <class T>
class MRSW
{
public:
    ~MRSW() { finish(); }

    void init(sched::Scheduler* s)
    {
        finish();
        sched_ = s;
        void* ptr = sched_->params().mem_callbacks.alloc_fn(sizeof(T));
        obj_ = new (ptr) T();
        read_mode_ = true;
    }

    void finish()
    {
        if (obj_)
        {
            sched_->waitFor(next_);
            obj_->~T();
            sched_->params().mem_callbacks.free_fn(obj_);
            obj_ = nullptr;
            sched_ = nullptr;
        }
    }

    template <class T_func>
    void executeRead(T_func func, sched::Sync* finish_signal = nullptr)
    {
        std::lock_guard<sched::Spinlock> g(lock_);
        if (!read_mode_)
        {
            read_mode_ = true;
            prev_ = next_;
            next_ = sched::Sync();
        }
        if (finish_signal)
            sched_->incrementSync(*finish_signal);
        sched_->runAfter(prev_,
                         [this, func, finish_signal]() {
                             func(static_cast<const T*>(obj_));
                             if (finish_signal)
                                 sched_->decrementSync(*finish_signal);
                         },
                         next_);
    }

    template <class T_func>
    void executeWrite(T_func func, sched::Sync* finish_signal = nullptr)
    {
        std::lock_guard<sched::Spinlock> g(lock_);
        read_mode_ = false;
        sched::Sync new_next;
        if (finish_signal)
            sched_->incrementSync(*finish_signal);
        sched_->runAfter(next_,
                         [this, func, finish_signal]() {
                             func(obj_);
                             if (finish_signal)
                                 sched_->decrementSync(*finish_signal);
                         },
                         new_next);
        next_ = new_next;
    }

private:
    sched::Scheduler* sched_ = nullptr;
    T* obj_ = nullptr;
    sched::Sync prev_;
    sched::Sync next_;
    sched::Spinlock lock_;
    bool read_mode_;
};

struct Example
{
    mutable std::atomic<int32_t> readers = {0};
    std::atomic<int32_t> writers = {0};
};

int main()
{
    atexit(mem_report);
    sched::Scheduler sched_;
    sched::SchedulerParams s_params;
    s_params.max_number_tasks = 8196;
    s_params.mem_callbacks.alloc_fn = mem_check_alloc;
    s_params.mem_callbacks.free_fn = mem_check_free;
    sched_.init(s_params);

    MRSW<Example> example;
    example.init(&sched_);

    for (uint32_t i = 0; i < 1000; ++i)
    {
        if ((std::rand() & 0xFF) < 200)
        {
            example.executeRead([i](const Example* e) {
                e->readers.fetch_add(1);
                printf("[%u] Read Op  %d(R)/%d(W)\n", i, e->readers.load(), e->writers.load());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                e->readers.fetch_sub(1);
            });
        }
        else
        {
            example.executeWrite([i](Example* e) {
                e->writers.fetch_add(1);
                printf("[%u] Write Op %d(R)/%d(W)\n", i, e->readers.load(), e->writers.load());
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                e->writers.fetch_sub(1);
            });
        }
    }

    printf("WAITING FOR TASKS TO FINISH....\n");

    return 0;
}
