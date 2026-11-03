add_library(core_dev STATIC src/dev.cpp src/host_driver.cpp src/cuda_driver.cpp)
add_library(core::dev ALIAS core_dev)
target_include_directories(core_dev PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(core_dev PUBLIC cxx_std_17)
# The CUDA runtime is loaded with dlopen, never linked.
target_link_libraries(core_dev PUBLIC core::mem core::sys core::base ${CMAKE_DL_LIBS})
core_target_defaults(core_dev)

if(CORE_BUILD_TESTS)
    add_executable(dev_test_dev tests/test_dev.cpp)
    target_link_libraries(dev_test_dev PRIVATE core::dev)
    target_include_directories(dev_test_dev PRIVATE ${CMAKE_SOURCE_DIR}/libbase/tests)
    core_target_defaults(dev_test_dev)
    add_test(NAME dev_dev COMMAND dev_test_dev)

    # Real CUDA runtime (dlopen); exits 77 = skipped on machines without libcudart or a device.
    add_executable(dev_test_cuda tests/test_cuda.cpp)
    target_link_libraries(dev_test_cuda PRIVATE core::dev)
    target_include_directories(dev_test_cuda PRIVATE ${CMAKE_SOURCE_DIR}/libbase/tests)
    core_target_defaults(dev_test_cuda)
    add_test(NAME dev_cuda COMMAND dev_test_cuda)
    set_tests_properties(dev_cuda PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
endif()
