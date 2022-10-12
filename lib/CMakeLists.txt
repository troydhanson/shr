cmake_minimum_required(VERSION 3.11.4)
set(CMAKE_CXX_STANDARD 11)

# libbw is a convenience library.
# Build as static, do not install.
# PIC for inclusion in shared lib.
add_library(bw STATIC EXCLUDE_FROM_ALL bw.c bw.h ux.c)
set_target_properties(bw PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Add ourself as include directory to user (shr)
target_include_directories(bw INTERFACE
 $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)


# bw-tool dev utility; do not install
add_executable(bw-tool bw-tool.c)
target_link_libraries(bw-tool bw)
