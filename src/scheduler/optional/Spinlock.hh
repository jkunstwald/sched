#pragma once

#include <thread>

#include <scheduler/Scheduler.hh>

namespace sched
{
//-- Optional: Spinlock ------------------------------------------------------
class Spinlock
{
public:
    Spinlock() : owner_(std::thread::id()), count_(0) {}

    ~Spinlock() { lock(); }

    void lock()
    {
        while (!try_lock())
        {
        }
    }

    void unlock()
    {
        std::thread::id tid = std::this_thread::get_id();
        SCHED_RUNTIME_ASSERT(owner_ == tid, "Invalid Spinlock::unlock owner mistmatch");
        count_--;
        if (count_ == 0)
        {
            owner_ = std::thread::id();
        }
    }

    bool try_lock()
    {
        std::thread::id tid = std::this_thread::get_id();
        if (owner_ == tid)
        {
            count_++;
            return true;
        }
        std::thread::id expected;
        if (owner_.compare_exchange_weak(expected, tid))
        {
            count_ = 1;
            return true;
        }
        return false;
    }

private:
    std::atomic<std::thread::id> owner_;
    uint32_t count_;
};
}
