#pragma once

/// -- Job Object ----------------------------------------------------------
///
/// If you want to use your own job objects, define the following
/// macro and provide a struct sched::Job object with an operator() method.
///
/// Example, a C-like pointer to function:
///
///    #define SCHED_CUSTOM_JOB_DEFINITION
///    namespace sched {
///      struct Job {
///        void (*func)(void *arg);
///        void *arg;
///        void operator()() { func(arg); }
///      };
///    } // sched namespace
///
///  By default Jobs are simply std::function<void()>
///
#ifndef SCHED_CUSTOM_JOB_DEFINITION
#include <functional>
namespace sched
{
typedef std::function<void()> Job;
}
#endif

#include <atomic>
#include <condition_variable>
#include <thread>

#include "common/check.hh"
#include "common/mem_callbacks.hh"
#include "object_pool.hh"

namespace sched
{
/// Sync object
class Sync
{
    uint32_t hnd = 0;
    friend class Scheduler;
};


struct SchedulerParams
{
    uint16_t num_threads = 16;                                                                 ///< num OS threads created
    uint16_t max_running_threads = static_cast<uint16_t>(std::thread::hardware_concurrency()); ///< 0 --> will be set to max hardware concurrency
    uint16_t max_number_tasks = 1024;                                                          ///< max number of simultaneous tasks
    uint16_t thread_num_tries_on_idle = 16;                                                    ///< number of tries before suspend the thread
    uint32_t thread_sleep_on_idle_in_microseconds = 5;                                         ///< time spent waiting between tries
    MemCallbacks mem_callbacks;
};


class Scheduler
{
public:
    Scheduler(const SchedulerParams& params = SchedulerParams());
    ~Scheduler();

    void init(const SchedulerParams& params = SchedulerParams());
    void stop();

    void run(const Job& job, Sync* out_sync_obj = nullptr);
    void runAfter(Sync sync, const Job& job, Sync* out_sync_obj = nullptr);
    void waitFor(Sync sync); ///< suspend current thread

    // returns the number of tasks not yet finished associated to the sync object
    // thus 0 means all of them has finished (or the sync object was empty, or
    // unused)
    uint32_t numPendingTasks(Sync s) const;

    bool hasFinished(Sync s) const { return numPendingTasks(s) == 0; }

    // Call this only to print the internal state of the scheduler, mainly if it
    // stops working and want to see who is waiting for what, and so on.
    void getDebugStatus(char* buffer, size_t buffer_size);

    // manually increment the value of a Sync object. Sync objects triggers
    // when they reach 0.
    // *WARNING*: calling increment without a later decrement might leave
    //            tasks waiting forever, and will leak resources.
    void incrementSync(Sync* s);

    // manually decrement the value of a Sync object.
    // *WARNING*: never call decrement on a sync object wihtout calling
    //            @incrementSync first.
    void decrementSync(Sync* s);

    // By default workers will be named as Worker-id
    // The pointer passed here must be valid until set_current_thread is called
    // again...
    static void set_current_thread_name(const char* name);
    static const char* current_thread_name();

    // Call this method before a mutex/lock/etc... to notify the scheduler
    static void CurrentThreadSleeps();

    // call this again to notify the thread is again running
    static void CurrentThreadWakesUp();

    // Call this method before locking a resource, this will be used by the
    // scheduler to wakeup another thread as a worker, and also can be used
    // later to detect deadlocks (if compiled with PX_SCHED_CHECK_DEADLOCKS 1,
    // WIP!)
    static void CurrentThreadBeforeLockResource(const void* resource_ptr, const char* name = nullptr);

    // Call this method after calling CurrentThreadBeforeLockResource, this will be
    // used to notify the scheduler that this thread can continue working.
    // If success is true, the lock was successful, false if the thread was not
    // blocked but also didn't adquired the lock (try_lock)
    static void CurrentThreadAfterLockResource(bool success);

    // Call this method once the resouce is unlocked.
    static void CurrentThreadReleasesResource(const void* resource_ptr);

    const SchedulerParams& params() const { return params_; }

    // Number of active threads (executing tasks)
    uint32_t active_threads() const { return active_threads_.load(); }

private:
    struct TLS;
    static TLS& tls();
    void wakeUpOneThread();
    SchedulerParams params_;
    std::atomic<uint32_t> active_threads_;
    std::atomic<uint32_t> running_ = {0};

    struct IndexQueue
    {
        ~IndexQueue() { SCHED_RUNTIME_ASSERT(list_ == nullptr, "IndexQueue Resources leaked..."); }
        void reset()
        {
            if (list_)
            {
                mem_.free_fn(list_);
                list_ = nullptr;
            }
            size_ = 0;
            in_use_ = 0;
        }
        void init(uint16_t max, const MemCallbacks& mem_cb = MemCallbacks())
        {
            _lock();
            reset();
            mem_ = mem_cb;
            size_ = max;
            in_use_ = 0;
            list_ = static_cast<uint32_t*>(mem_.alloc_fn(sizeof(uint32_t) * size_));
            _unlock();
        }
        void push(uint32_t p)
        {
            _lock();
            SCHED_RUNTIME_ASSERT(in_use_ < size_, "IndexQueue Overflow total in use %hu (max %hu)", in_use_, size_);
            uint16_t pos = (current_ + in_use_) % size_;
            list_[pos] = p;
            in_use_++;
            _unlock();
        }
        uint16_t in_use()
        {
            _lock();
            uint16_t result = in_use_;
            _unlock();
            return result;
        }
        bool pop(uint32_t* res)
        {
            _lock();
            bool result = false;
            if (in_use_)
            {
                if (res)
                    *res = list_[current_];
                current_ = (current_ + 1) % size_;
                in_use_--;
                result = true;
            }
            _unlock();
            return result;
        }
        void _unlock() { lock_.clear(std::memory_order_release); }
        void _lock()
        {
            while (lock_.test_and_set(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }
        uint32_t* list_ = nullptr;
        std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
        MemCallbacks mem_;
        volatile uint16_t size_ = 0;
        volatile uint16_t in_use_ = 0;
        volatile uint16_t current_ = 0;
    };

    struct Task
    {
        Job job;
        uint32_t counter_id = 0;
        std::atomic<uint32_t> next_sibling_task = {0};
    };

    struct WaitFor
    {
        explicit WaitFor() : owner(std::this_thread::get_id()), ready(false) {}
        void wait()
        {
            SCHED_RUNTIME_ASSERT(std::this_thread::get_id() == owner, "WaitFor::wait can only be invoked from the thread "
                                                                      "that created the object");
            std::unique_lock<std::mutex> lk(mutex);
            if (!ready)
            {
                condition_variable.wait(lk);
            }
        }
        void signal()
        {
            if (owner != std::this_thread::get_id())
            {
                std::lock_guard<std::mutex> lk(mutex);
                ready = true;
                condition_variable.notify_all();
            }
            else
            {
                ready = true;
            }
        }

    private:
        std::thread::id const owner;
        std::mutex mutex;
        std::condition_variable condition_variable;
        bool ready;
    };

    struct Worker
    {
        std::thread thread;
        // setted by the thread when is sleep
        std::atomic<WaitFor*> wake_up = {nullptr};
        TLS* thread_tls = nullptr;
        uint16_t thread_index = 0xFFFF;
    };

    struct Counter
    {
        std::atomic<uint32_t> task_id;
        std::atomic<uint32_t> user_count;
        WaitFor* wait_ptr = nullptr;
    };

    uint16_t wakeUpThreads(uint16_t max_num_threads);
    uint32_t createTask(const Job& job, Sync* out_sync_obj);
    uint32_t createCounter();
    void unrefCounter(uint32_t counter_hnd);

    Worker* workers_ = nullptr;
    ObjectPool<Task> tasks_;
    ObjectPool<Counter> counters_;
    IndexQueue ready_tasks_;

    static void WorkerThreadMain(Scheduler* schd, Worker*);
};

}
