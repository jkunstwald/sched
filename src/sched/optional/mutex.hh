#pragma once

#include <thread>

#include <sched/scheduler.hh>

namespace sched
{
/// Mutex template to encapsultae scheduler notification
/// Optional, not used in this library
template <class M>
class Mutex
{
public:
    ~Mutex() { lock(); }

    void lock()
    {
        std::thread::id tid = std::this_thread::get_id();
        if (owner_ == tid)
        {
            count_++;
            return;
        }
        Scheduler::CurrentThreadBeforeLockResource(&mutex_);
        mutex_.lock();
        owner_ = tid;
        count_ = 1;
        Scheduler::CurrentThreadAfterLockResource(true);
    }
    void unlock()
    {
        std::thread::id tid = std::this_thread::get_id();
        SCHED_RUNTIME_ASSERT(owner_ == tid, "Invalid Mutex::unlock owner mistmatch");
        count_--;
        if (count_ == 0)
        {
            owner_ = std::thread::id();
            Scheduler::CurrentThreadReleasesResource(&mutex_);
            mutex_.unlock();
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
        Scheduler::CurrentThreadBeforeLockResource(&mutex_);
        bool result = mutex_.try_lock();
        if (result)
        {
            owner_ = tid;
            count_ = 1;
        }
        Scheduler::CurrentThreadAfterLockResource(result);
        return result;
    }

private:
    std::thread::id owner_;
    uint32_t count_ = 0;
    M mutex_;
};
}
