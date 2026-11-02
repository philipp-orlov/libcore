add_library(develop_dev STATIC src/dev.cpp src/host_driver.cpp src/cuda_driver.cpp)
add_library(develop::dev ALIAS develop_dev)
target_include_directories(develop_dev PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(develop_dev PUBLIC cxx_std_17)
# The CUDA runtime is loaded with dlopen, never linked.
target_link_libraries(develop_dev PUBLIC develop::mem develop::sys develop::base ${CMAKE_DL_LIBS})
develop_target_defaults(develop_dev)

if(DEVELOP_BUILD_TESTS)
    add_executable(dev_test_dev tests/test_dev.cpp)
    target_link_libraries(dev_test_dev PRIVATE develop::dev)
    target_include_directories(dev_test_dev PRIVATE ${CMAKE_SOURCE_DIR}/libbase/tests)
    develop_target_defaults(dev_test_dev)
    add_test(NAME dev_dev COMMAND dev_test_dev)

    # Real CUDA runtime (dlopen); exits 77 = skipped on machines without libcudart or a device.
    add_executable(dev_test_cuda tests/test_cuda.cpp)
    target_link_libraries(dev_test_cuda PRIVATE develop::dev)
    target_include_directories(dev_test_cuda PRIVATE ${CMAKE_SOURCE_DIR}/libbase/tests)
    develop_target_defaults(dev_test_cuda)
    add_test(NAME dev_cuda COMMAND dev_test_cuda)
    set_tests_properties(dev_cuda PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
endif()
