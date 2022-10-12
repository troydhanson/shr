cmake_minimum_required(VERSION 3.11.4)
set(CMAKE_CXX_STANDARD 11)

# shr library
# link with libbw.a
add_library(shr SHARED shr.c)
target_link_libraries(shr bw)

# install shr library and its public header
set_target_properties(shr PROPERTIES PUBLIC_HEADER shr.h)
install(TARGETS shr)
# run ldconfig
if (UNIX)
  install(CODE "execute_process(COMMAND ldconfig)")
endif()

