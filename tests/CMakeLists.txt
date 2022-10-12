cmake_minimum_required(VERSION 3.11.4)
set(CMAKE_CXX_STANDARD 11)

file(GLOB tests test*.c)
foreach (test IN LISTS tests)

  # Convert /abs/path/to/test10.c to test10.c */
  file(RELATIVE_PATH test ${CMAKE_CURRENT_SOURCE_DIR} ${test})
  # Form name of test executable; test10.c becomes test10 */
  string(REGEX MATCH "^test[0-9]+" exe ${test})

  list(APPEND exes ${exe})
  add_executable(${exe} ${test})
  target_link_libraries(${exe} shr)
endforeach()

add_custom_target(run-tests
  DEPENDS ${exes}
  COMMAND ./do_tests.py ${CMAKE_CURRENT_BINARY_DIR}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
