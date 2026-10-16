# Unit tests for the plugin's libobs-free code. A project of its OWN, deliberately: it needs neither
# libobs nor the OBS plugin template, so it builds anywhere a C compiler does.
#
#   cmake -S tests -B build_tests
#   cmake --build build_tests --config Release
#   ctest --test-dir build_tests -C Release --output-on-failure

cmake_minimum_required(VERSION 3.20)

project(castbeacon-tests LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

find_package(Threads REQUIRED)

enable_testing()

function(castbeacon_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE ../src)
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /WX)
  else()
    target_compile_options(
      ${name}
      PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
    )
  endif()
  # the macOS plugin build treats a missing final newline as an error: so do the tests
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(${name} PRIVATE -Wnewline-eof)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# The decision itself: pure, no OS calls.
castbeacon_test(stand-aside-test stand-aside-test.c ../src/stand-aside.c)

# The threaded lookup and the decision end to end, against a fake resolver.
castbeacon_test(
  local-host-test
  local-host-test.c
  ../src/local-host.c
  ../src/cb-thread.c
  ../src/stand-aside.c
)
target_link_libraries(local-host-test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
  target_link_libraries(local-host-test PRIVATE iphlpapi ws2_32)
endif()

# The shadow output's state machine, against a fake OBS (shadow, UI queue, judge, pin).
castbeacon_test(
  control-test
  control-test.c
  ../src/shadow-control.c
  ../src/local-host.c
  ../src/cb-thread.c
  ../src/stand-aside.c
)
target_link_libraries(control-test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
  target_link_libraries(control-test PRIVATE iphlpapi ws2_32)
endif()
