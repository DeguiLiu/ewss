cmake_minimum_required(VERSION 3.14)
project(ewss VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(EWSS_BUILD_TESTS "Build tests" ON)
option(EWSS_BUILD_EXAMPLES "Build examples" ON)
option(EWSS_NO_EXCEPTIONS "Build without exceptions" OFF)

# Compiler flags
if(EWSS_NO_EXCEPTIONS)
  add_compile_options(-fno-exceptions -fno-rtti)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  add_compile_options(-Werror=format -Werror=unused-result)
endif()

# FetchContent for dependencies
include(FetchContent)

# sockpp: TCP socket wrapper
FetchContent_Declare(
  sockpp
  GIT_REPOSITORY https://ghfast.top/https://github.com/fpagliughi/sockpp.git
  GIT_TAG v0.7.1
)

FetchContent_MakeAvailable(sockpp)

# Main library (header-only with some .cpp files)
file(GLOB SOURCE_FILES "src/*.cpp")

add_library(ewss ${SOURCE_FILES})
target_include_directories(ewss PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(ewss PUBLIC sockpp)
target_compile_features(ewss PUBLIC cxx_std_17)

# Tests
if(EWSS_BUILD_TESTS)
  enable_testing()

  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://ghfast.top/https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
  )
  FetchContent_MakeAvailable(Catch2)

  file(GLOB TEST_SOURCES "tests/test_*.cpp")

  foreach(test_src ${TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE ewss Catch2::Catch2WithMain)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()

# Examples
if(EWSS_BUILD_EXAMPLES)
  file(GLOB EXAMPLE_SOURCES "examples/*.cpp")

  foreach(example_src ${EXAMPLE_SOURCES})
    get_filename_component(example_name ${example_src} NAME_WE)
    add_executable(${example_name} ${example_src})
    target_link_libraries(${example_name} PRIVATE ewss)
  endforeach()
endif()

# Installation
include(GNUInstallDirs)
install(DIRECTORY include/ewss DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
