cmake_minimum_required(VERSION 3.11.4)
project(SHR)
set(CMAKE_CXX_STANDARD 11)

add_subdirectory(lib)
add_subdirectory(src)
add_subdirectory(util)

# "make test" builds and executes them
add_subdirectory(tests EXCLUDE_FROM_ALL)
add_custom_target(test DEPENDS run-tests)
