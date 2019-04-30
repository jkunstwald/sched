#pragma once

#include <cstdlib>

namespace sched
{
struct MemCallbacks
{
    void* (*alloc_fn)(size_t amount) = ::malloc;
    void (*free_fn)(void* ptr) = ::free;
};
}
