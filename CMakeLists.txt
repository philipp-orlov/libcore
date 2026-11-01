cmake_minimum_required(VERSION 3.16)
project(develop_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/buildkit/cmake")
include(DevelopCompiler)
include(DevelopSanitize)

option(DEVELOP_BUILD_TESTS "Build the tests of every component" ON)
if(DEVELOP_BUILD_TESTS)
    include(CTest)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# One directory per future repository; each is self-contained and depends only on the ones before it.
add_subdirectory(libbase)
add_subdirectory(libsys)
add_subdirectory(libmem)
add_subdirectory(libsched)
add_subdirectory(liblog)
add_subdirectory(libtar)
add_subdirectory(libcfg)
add_subdirectory(libbench)
