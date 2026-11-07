cmake_minimum_required(VERSION 3.16)
project(libcore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/buildkit/cmake")
include(CoreCompiler)
include(CoreSanitize)

option(CORE_BUILD_TESTS "Build the tests of every component" ON)
if(CORE_BUILD_TESTS)
    include(CTest)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# One directory per future repository; each is self-contained and depends only on the ones before it.
#
# Inside another project, a component is built when something links it and not merely because it exists: a consumer of
# core::sys has no use for the hardware probes or the CUDA driver wrapper, and on a platform where those do not build
# (Windows, so far) must not fail on them. libcore built on its own still builds everything.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(CORE_SUBDIR_OPTIONS "")
else()
    set(CORE_SUBDIR_OPTIONS EXCLUDE_FROM_ALL)
endif()
foreach(component base sys mem sched log tar cfg schema json hw dev net bench)
    add_subdirectory(lib${component} ${CORE_SUBDIR_OPTIONS})
endforeach()

# ---- install and export ------------------------------------------------------------------------------------------
# `find_package(libcore CONFIG)` then link core::base, core::sys, core::mem, ...
option(CORE_INSTALL "Install the libraries, headers and CMake package" ${PROJECT_IS_TOP_LEVEL})
if(CORE_INSTALL)
    include(GNUInstallDirs)
    set(_core_components base sys mem sched log json tar schema cfg hw dev net bench)
    set(_core_targets "")
    foreach(c IN LISTS _core_components)
        if(TARGET core_${c})
            set_target_properties(core_${c} PROPERTIES EXPORT_NAME ${c})
            target_include_directories(core_${c} PUBLIC $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
            list(APPEND _core_targets core_${c})
            file(GLOB _dir LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/lib${c}/include")
            if(_dir)
                install(DIRECTORY lib${c}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
            endif()
        endif()
    endforeach()
    install(TARGETS ${_core_targets} EXPORT core-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT core-targets NAMESPACE core:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libcore)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/libcoreConfig.cmake"
"include(CMakeFindDependencyMacro)
find_dependency(Threads)
include(\"\${CMAKE_CURRENT_LIST_DIR}/core-targets.cmake\")
list(APPEND CMAKE_MODULE_PATH \"\${CMAKE_CURRENT_LIST_DIR}\")
")
    include(CMakePackageConfigHelpers)
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/libcoreConfigVersion.cmake"
                                     VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libcoreConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/libcoreConfigVersion.cmake"
                  buildkit/cmake/CoreCompiler.cmake buildkit/cmake/CoreSanitize.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libcore)
endif()
