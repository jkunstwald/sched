cmake_minimum_required(VERSION 3.0)
project(sched)

#
# Options
#

set(SCHED_BUILD_EXAMPLE OFF CACHE BOOL "If true, an example executable is built")
set(SCHED_RUNTIME_CHECKS OFF CACHE BOOL "If true, some runtime checks are performed")

#
# Library
#

file(GLOB_RECURSE SOURCE_FILES "src/*.cc")
file(GLOB_RECURSE HEADER_FILES "src/*.hh")

add_library(sched STATIC ${SOURCE_FILES} ${HEADER_FILES})
target_include_directories(sched PUBLIC "src")

if (SCHED_RUNTIME_CHECKS)
    target_compile_definitions(sched PUBLIC SCHED_RUNTIME_CHECKS)
endif()

#
# Example
#

if (SCHED_BUILD_EXAMPLE)
    add_executable(example "example/example.cc")
    target_link_libraries(example PUBLIC sched)
endif()
