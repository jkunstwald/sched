cmake_minimum_required(VERSION 3.0)
project(Scheduler)

#
# Library
#

file(GLOB_RECURSE SOURCE_FILES "src/*.cc")
file(GLOB_RECURSE HEADER_FILES "src/*.hh")

add_library(scheduler STATIC ${SOURCE_FILES} ${HEADER_FILES})
target_include_directories(scheduler PUBLIC "src")

#
# Example
#

set(SCHED_BUILD_EXAMPLE OFF CACHE BOOL "If true, an example executable is built")
if (SCHED_BUILD_EXAMPLE)
    add_executable(example "example/example.cc")
    target_link_libraries(example PUBLIC scheduler)
endif()
