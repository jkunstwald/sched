#include "scheduler.hh"

#include <thread>

namespace sched
{
struct Scheduler::TLS
{
    const char* name = nullptr;
    Scheduler* scheduler = nullptr;
    struct Resource
    {
        const void* ptr;
        const char* name;
    };
    Resource next_lock = {nullptr, nullptr};
};

Scheduler::TLS& Scheduler::tls()
{
    static thread_local TLS tls;
    return tls;
}

void Scheduler::set_current_thread_name(const char* name)
{
    tls().name = name;
}

const char* Scheduler::current_thread_name()
{
    return tls().name;
}

void Scheduler::CurrentThreadSleeps()
{
    CurrentThreadBeforeLockResource(nullptr);
}

void Scheduler::CurrentThreadWakesUp()
{
    CurrentThreadAfterLockResource(false);
}

void Scheduler::CurrentThreadBeforeLockResource(const void* resource_ptr, const char* name)
{
    // if the lock might work, wake up one thread to replace this one
    TLS& d = tls();
    if (d.scheduler && d.scheduler->running_.load())
    {
        d.scheduler->active_threads_.fetch_sub(1);
        d.scheduler->wakeUpOneThread();
    }
    d.next_lock = {resource_ptr, name};
}

void Scheduler::CurrentThreadAfterLockResource(bool success)
{
    // mark this thread as active (so eventually one thread will step down)
    TLS& d = tls();
    if (d.scheduler && d.scheduler->running_.load())
    {
        d.scheduler->active_threads_.fetch_add(1);
    }
    if (success && d.next_lock.ptr)
    {
    }
    d.next_lock = {nullptr, nullptr}; // reset
}

void Scheduler::CurrentThreadReleasesResource(const void* resource_ptr)
{
    (void)resource_ptr;
}
}

namespace sched
{
Scheduler::Scheduler(const SchedulerParams& _params)
{
    active_threads_ = 0;
    init(_params);
}

Scheduler::~Scheduler()
{
    stop();
}

void Scheduler::init(const SchedulerParams& _params)
{
    stop();
    running_ = true;
    params_ = _params;
    if (params_.max_running_threads == 0)
        params_.max_running_threads = static_cast<uint16_t>(std::thread::hardware_concurrency());

    // create tasks
    tasks_.init(params_.max_number_tasks, params_.mem_callbacks);
    counters_.init(params_.max_number_tasks, params_.mem_callbacks);
    ready_tasks_.init(params_.max_number_tasks, params_.mem_callbacks);
    SCHED_RUNTIME_ASSERT(workers_ == nullptr, "workers_ ptr should be null here...");
    workers_ = static_cast<Worker*>(params_.mem_callbacks.alloc_fn(sizeof(Worker) * params_.num_threads));
    for (uint16_t i = 0; i < params_.num_threads; ++i)
    {
        new (&workers_[i]) Worker();
        workers_[i].thread_index = i;
    }
    SCHED_RUNTIME_ASSERT(active_threads_.load() == 0, "Invalid active threads num");
    for (uint16_t i = 0; i < params_.num_threads; ++i)
    {
        workers_[i].thread = std::thread(WorkerThreadMain, this, &workers_[i]);
    }
}

void Scheduler::stop()
{
    if (running_)
    {
        running_ = false;
        for (uint16_t i = 0; i < params_.num_threads; ++i)
        {
            wakeUpThreads(params_.num_threads);
        }
        for (uint16_t i = 0; i < params_.num_threads; ++i)
        {
            workers_[i].thread.join();
            workers_[i].~Worker();
        }
        params_.mem_callbacks.free_fn(workers_);
        workers_ = nullptr;
        tasks_.reset();
        counters_.reset();
        ready_tasks_.reset();
        SCHED_RUNTIME_ASSERT(active_threads_.load() == 0, "Invalid active threads num --> %u", active_threads_.load());
    }
}

void Scheduler::getDebugStatus(char* buffer, size_t buffer_size)
{
    size_t p = 0;
    int n = 0;
#define ADD_OUTPUT(...)                                                                \
    {                                                                                  \
        p += static_cast<size_t>(n);                                                   \
        (p < buffer_size) && (n = snprintf(buffer + p, buffer_size - p, __VA_ARGS__)); \
    }
    ADD_OUTPUT("Workers:0    5    10   15   20   25   30   35   40   45   50   55   60   65   70   75\n");
    ADD_OUTPUT("%3u/%3u:", active_threads_.load(), params_.max_running_threads);
    for (size_t i = 0; i < params_.num_threads; ++i)
    {
        ADD_OUTPUT((workers_[i].wake_up.load() == nullptr) ? "*" : ".");
    }
    ADD_OUTPUT("\nWorkers(%d):", params_.num_threads);
    for (size_t i = 0; i < params_.num_threads; ++i)
    {
        auto& w = workers_[i];
        bool is_on = (w.wake_up.load() == nullptr);
        bool has_something_to_show = w.thread_tls->next_lock.ptr;
        if (!is_on && !has_something_to_show)
        {
            continue;
        }
        ADD_OUTPUT("\n  Worker: %d(%s) %s", w.thread_index, is_on ? "ON" : "OFF", w.thread_tls->name ? w.thread_tls->name : "-no-name-");
        if (w.thread_tls->next_lock.ptr)
        {
            if (w.thread_tls->next_lock.name)
            {
                ADD_OUTPUT("\n    Waiting For Lock: %p(%s)", w.thread_tls->next_lock.ptr, w.thread_tls->next_lock.name);
            }
            else
            {
                ADD_OUTPUT("\n    Waiting For Lock: %p", w.thread_tls->next_lock.ptr);
            }
        }
    }
    ADD_OUTPUT("\nReady: ");
    for (size_t i = 0; i < ready_tasks_.in_use_; ++i)
    {
        ADD_OUTPUT("%d,", ready_tasks_.list_[i]);
    }
    ADD_OUTPUT("\nTasks: ");
    for (size_t i = 0; i < tasks_.size(); ++i)
    {
        uint32_t c, v;
        uint32_t hnd = tasks_.info(i, &c, &v);
        if (c > 0)
        {
            ADD_OUTPUT("%u,", hnd);
        }
    }
    ADD_OUTPUT("\nCounters:");
    for (size_t i = 0; i < counters_.size(); ++i)
    {
        uint32_t c, v;
        uint32_t hnd = counters_.info(i, &c, &v);
        if (c > 0)
        {
            ADD_OUTPUT("%u,", hnd);
        }
    }
    ADD_OUTPUT("\n");
#undef ADD_OUTPUT
}

uint32_t Scheduler::createCounter()
{
    uint32_t hnd = counters_.acquireAndRef();
    auto& c = counters_.get(hnd);
    c.task_id = 0;
    c.user_count = 0;
    c.wait_ptr = nullptr;
    return hnd;
}

uint32_t Scheduler::createTask(const Job& job, Sync* sync_obj)
{
    uint32_t ref = tasks_.acquireAndRef();
    auto& task = tasks_.get(ref);
    task.job = job;
    task.counter_id = 0;
    task.next_sibling_task = 0;
    if (sync_obj)
    {
        bool new_counter = !counters_.ref(sync_obj->hnd);
        if (new_counter)
        {
            sync_obj->hnd = createCounter();
        }
        task.counter_id = sync_obj->hnd;
    }
    return ref;
}

uint16_t Scheduler::wakeUpThreads(uint16_t max_num_threads)
{
    uint16_t total_woken_up = 0;
    for (uint32_t i = 0; (i < params_.num_threads) && (total_woken_up < max_num_threads); ++i)
    {
        WaitFor* wake_up = workers_[i].wake_up.exchange(nullptr);
        if (wake_up)
        {
            wake_up->signal();
            total_woken_up++;
            // Add one to the total active threads, for later substracting it, this
            // will take the thread as awake before the thread actually is again working
            active_threads_.fetch_add(1);
        }
    }
    active_threads_.fetch_sub(total_woken_up);
    return total_woken_up;
}

void Scheduler::wakeUpOneThread()
{
    for (;;)
    {
        uint32_t active = active_threads_.load();
        if ((active >= params_.max_running_threads) || wakeUpThreads(1))
            return;
    }
}

void Scheduler::run(const Job& job, Sync* sync_obj)
{
    SCHED_RUNTIME_ASSERT(running_, "Scheduler not running");
    uint32_t t_ref = createTask(job, sync_obj);
    ready_tasks_.push(t_ref);
    wakeUpOneThread();
}

void Scheduler::runAfter(Sync _trigger, const Job& _job, Sync* _sync_obj)
{
    SCHED_RUNTIME_ASSERT(running_, "Scheduler not running");
    uint32_t trigger = _trigger.hnd;
    uint32_t t_ref = createTask(_job, _sync_obj);
    bool valid = counters_.ref(trigger);
    if (valid)
    {
        Counter* c = &counters_.get(trigger);
        for (;;)
        {
            uint32_t current = c->task_id.load();
            if (c->task_id.compare_exchange_strong(current, t_ref))
            {
                Task* task = &tasks_.get(t_ref);
                task->next_sibling_task = current;
                break;
            }
        }
        unrefCounter(trigger);
    }
    else
    {
        ready_tasks_.push(t_ref);
        wakeUpOneThread();
    }
}

void Scheduler::waitFor(Sync s)
{
    if (counters_.ref(s.hnd))
    {
        Counter& counter = counters_.get(s.hnd);
        SCHED_RUNTIME_ASSERT(counter.wait_ptr == nullptr, "Sync object already used for waitFor operation, only one is permitted");
        WaitFor wf;
        counter.wait_ptr = &wf;
        unrefCounter(s.hnd);
        CurrentThreadSleeps();
        wf.wait();
        CurrentThreadWakesUp();
    }
}

uint32_t Scheduler::numPendingTasks(Sync s) const
{
    return counters_.refCount(s.hnd);
}

void Scheduler::unrefCounter(uint32_t hnd)
{
    if (counters_.ref(hnd))
    {
        counters_.unref(hnd);
        counters_.unref(hnd, [this](Counter& c) {
            // wake up all tasks
            uint32_t tid = c.task_id;
            while (this->tasks_.ref(tid))
            {
                Task& task = this->tasks_.get(tid);
                uint32_t next_tid = task.next_sibling_task;
                task.next_sibling_task = 0;
                this->ready_tasks_.push(tid);
                this->wakeUpOneThread();
                this->tasks_.unref(tid);
                tid = next_tid;
            }
            if (c.wait_ptr)
            {
                c.wait_ptr->signal();
            }
        });
    }
}

void Scheduler::incrementSync(Sync* s)
{
    bool new_counter = false;
    if (!counters_.ref(s->hnd))
    {
        s->hnd = createCounter();
        new_counter = true;
    }
    Counter& c = counters_.get(s->hnd);
    c.user_count.fetch_add(1);
    if (!new_counter)
    {
        unrefCounter(s->hnd);
    }
}

void Scheduler::decrementSync(Sync* s)
{
    if (counters_.ref(s->hnd))
    {
        Counter& c = counters_.get(s->hnd);
        uint32_t prev = c.user_count.fetch_sub(1);
        if (prev == 1)
        {
            // last one should unref twice
            unrefCounter(s->hnd);
        }
        unrefCounter(s->hnd);
    }
}

void Scheduler::WorkerThreadMain(Scheduler* schd, Scheduler::Worker* worker_data)
{
    char buffer[16];

    const uint16_t id = worker_data->thread_index;
    TLS& local_storage = tls();

    local_storage.scheduler = schd;
    worker_data->thread_tls = &local_storage;

    auto const ttl_wait = schd->params_.thread_sleep_on_idle_in_microseconds;
    auto const ttl_value = schd->params_.thread_num_tries_on_idle ? schd->params_.thread_num_tries_on_idle : 1;
    schd->active_threads_.fetch_add(1);
    snprintf(buffer, 16, "Worker-%u", id);
    schd->set_current_thread_name(buffer);
    for (;;)
    {
        { // wait for new activity
            auto current_num = schd->active_threads_.fetch_sub(1);
            if (!schd->running_)
                return;
            if (schd->ready_tasks_.in_use() == 0 || current_num > schd->params_.max_running_threads)
            {
                WaitFor wf;
                schd->workers_[id].wake_up = &wf;
                wf.wait();
                if (!schd->running_)
                    return;
            }
            schd->active_threads_.fetch_add(1);
            schd->workers_[id].wake_up = nullptr;
        }
        auto ttl = ttl_value;
        { // do some work
            uint32_t task_ref;
            while (ttl && schd->running_)
            {
                if (!schd->ready_tasks_.pop(&task_ref))
                {
                    ttl--;
                    if (ttl_wait)
                        std::this_thread::sleep_for(std::chrono::microseconds(ttl_wait));
                    continue;
                }
                ttl = ttl_value;
                Task* t = &schd->tasks_.get(task_ref);
                t->job();
                uint32_t counter = t->counter_id;
                schd->tasks_.unref(task_ref);
                schd->unrefCounter(counter);
            }
        }
    }
}
}
