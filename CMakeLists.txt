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
add_subdirectory(libschema)
add_subdirectory(libjson)
add_subdirectory(libhw)
add_subdirectory(libdev)
add_subdirectory(libnet)
add_subdirectory(libbench)

# ---- install and export ------------------------------------------------------------------------------------------
# `find_package(develop-core CONFIG)` then link develop::base, develop::sys, develop::mem, ...
option(DEVELOP_INSTALL "Install the libraries, headers and CMake package" ${PROJECT_IS_TOP_LEVEL})
if(DEVELOP_INSTALL)
    include(GNUInstallDirs)
    set(_develop_components base sys mem sched log json tar schema cfg hw dev net bench)
    set(_develop_targets "")
    foreach(c IN LISTS _develop_components)
        if(TARGET develop_${c})
            set_target_properties(develop_${c} PROPERTIES EXPORT_NAME ${c})
            target_include_directories(develop_${c} PUBLIC $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
            list(APPEND _develop_targets develop_${c})
            file(GLOB _dir LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/lib${c}/include")
            if(_dir)
                install(DIRECTORY lib${c}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
            endif()
        endif()
    endforeach()
    install(TARGETS ${_develop_targets} EXPORT develop-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT develop-targets NAMESPACE develop:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/develop-core)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/develop-coreConfig.cmake"
"include(CMakeFindDependencyMacro)
find_dependency(Threads)
include(\"\${CMAKE_CURRENT_LIST_DIR}/develop-targets.cmake\")
")
    include(CMakePackageConfigHelpers)
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/develop-coreConfigVersion.cmake"
                                     VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/develop-coreConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/develop-coreConfigVersion.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/develop-core)
endif()
