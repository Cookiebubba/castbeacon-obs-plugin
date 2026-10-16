# Unit tests for the plugin's pure logic. A project of its OWN, deliberately: it needs neither libobs
# nor the OBS plugin template, so it builds anywhere a C compiler does.
#
#   cmake -S tests -B build_tests
#   cmake --build build_tests --config Release
#   ctest --test-dir build_tests -C Release --output-on-failure

cmake_minimum_required(VERSION 3.20)

project(castbeacon-tests LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

enable_testing()

add_executable(stand-aside-test stand-aside-test.c ../src/stand-aside.c)
target_include_directories(stand-aside-test PRIVATE ../src)

if(MSVC)
  target_compile_options(stand-aside-test PRIVATE /W4 /WX)
else()
  target_compile_options(
    stand-aside-test
    PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
  )
endif()

add_test(NAME stand-aside COMMAND stand-aside-test)
