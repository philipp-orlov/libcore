add_library(core_hw STATIC
    src/line_stream.cpp src/tegrastats_provider.cpp src/nvidia_smi_provider.cpp src/nvml_provider.cpp
    src/proc_provider.cpp src/process_sampler.cpp src/network_sampler.cpp src/provider_factory.cpp src/log.cpp src/probe.cpp)
add_library(core::hw ALIAS core_hw)
target_include_directories(core_hw PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(core_hw PUBLIC cxx_std_17)
# NVML is loaded with dlopen at run time, never linked.
target_link_libraries(core_hw PUBLIC core::sys core::base ${CMAKE_DL_LIBS})
core_target_defaults(core_hw)

if(CORE_BUILD_TESTS)
    add_executable(hw_test_hw tests/test_hw.cpp)
    target_link_libraries(hw_test_hw PRIVATE core::hw)
    target_include_directories(hw_test_hw PRIVATE ${CMAKE_SOURCE_DIR}/libbase/tests ${CMAKE_SOURCE_DIR}/libbench/include)
    core_target_defaults(hw_test_hw)
    add_test(NAME hw_hw COMMAND hw_test_hw)
endif()
