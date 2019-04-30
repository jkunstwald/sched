# sched
Simple C++11 Task Scheduler, forked from [px_sched](https://github.com/pplux/px/blob/master/README_px_sched.md)

No dependencies and easy to integrate

```C++
#include <sched/scheduler.hh>

int main() {
    sched::Scheduler scheduler;

    sched::Sync stage1;

    for (auto i = 0; i < 500; ++i)
    {
        scheduler.run([] {
                log << "Job";
        }, stage1);
    }

    sched::Sync stage2;

    scheduler.runAfter(stage1, [] {
            log << "Job with dependency";
    }, stage2);

    scheduler.waitFor(stage2);
    log << "All jobs done";
}

```

Forked with the goals of a cleaner API, simple CMake integration, and avoiding issues with the single-header pattern.

## Goals

* Allow task oriented multihtread programmming without mutexes, locks, condition variables...
* Implicit task graph dependency for single or groups of tasks
* Portable, written in C++11 with no dependency
* C++11 only used for thread, mutex, and condition variable *no STL container is used*
* Easy to use, flexible, and lightweight
* Inspied by Naughty Dog's talk [Parallelizing the Naughty Dog Engine](https://www.gdcvault.com/play/1022186/Parallelizing-the-Naughty-Dog-Engine), [enkiTS](https://github.com/dougbinks/enkiTS), and [STB's single-file libraries](https://github.com/nothings/stb)
* No dynamic memory allocations, all memory is allocated at initialization. Easy to integrate with your own memory manager if needed.

## API

`sched::Scheduler` is the main object, usually just instantiated once. There are two methods to launch tasks:

```C++
// Dispatch jobs immediately
scheduler.run(job);
scheduler.run(job, s1);

// Dispatch jobs after a sync has finished
scheduler.runAfter(s1, job2);
scheduler.runAfter(s1, job2, s2);
```

The run methods receive a `Job` object, by default a `std::function<void()>` but you can define `SCHED_CUSTOM_JOB_DEFINITION` and provide a custom `sched::Job`.

Both run methods receive an optional argument, a `Sync` object. `Sync` objects are used to coordinate dependencies between groups of tasks (or single tasks). The simplest case is to wait for a group of tasks to finish:

```cpp
sched::Sync sync;
for(size_t i = 0; i < 128; ++i) {
    scheduler.run([i]{ doWork(i); }, sync);
}
schd.waitFor(sync);
```

`Sync` objects can also be used to launch tasks when a group of tasks have finished with the method `sched::Scheduler::runAfter`:

```cpp
sched::Sync s1;
for(size_t i = 0; i < 128; ++i) {
    scheduler.run([i]{ doWork(i); }, s1);
}
sched::Sync s2;
scheduler.runAfter(s1, []{ doDependentWork(); }, s2);

scheduler.waitFor(s2);
```
