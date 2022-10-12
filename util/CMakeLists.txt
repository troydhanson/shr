cmake_minimum_required(VERSION 3.11.4)
set(CMAKE_CXX_STANDARD 11)

# shr-tool utility
add_executable(shr-tool shr-tool.c)
target_link_libraries(shr-tool shr)

# install utility
install(TARGETS shr-tool)
