cmake_minimum_required(VERSION 3.0)
project(Scheduler)

file(GLOB_RECURSE SOURCE_FILES "src/*.cc")
file(GLOB_RECURSE HEADER_FILES "src/*.hh")

#add_subdirectory(extern/boost_context)

add_library(scheduler STATIC ${SOURCE_FILES} ${HEADER_FILES})
#target_link_libraries(scheduler PUBLIC boost_context)
target_include_directories(scheduler PUBLIC "src")

add_executable(example "example/example.cc")
target_link_libraries(example PUBLIC scheduler)
