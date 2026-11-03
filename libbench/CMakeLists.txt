add_library(core_bench STATIC soak/soak.cpp)
add_library(core::bench ALIAS core_bench)
target_include_directories(core_bench PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(core_bench PUBLIC cxx_std_17)
target_link_libraries(core_bench PUBLIC core::sys core::base)
core_target_defaults(core_bench)

if(CORE_BUILD_TESTS)
    add_executable(bench_soak_memory soak/soak_memory.cpp)
    target_link_libraries(bench_soak_memory PRIVATE core::bench core::mem core::sched core::log)
    core_target_defaults(bench_soak_memory)
    # Sanitizers slow the size drift of this workload so much that it never reaches a steady state in a short run, so
    # the soak is registered for unsanitized builds only. 30 seconds: in 12 the pool gauges are still settling and read
    # as growth about once in 50 runs. Set CORE_SOAK_SECONDS=86400 for a real one.
    if(NOT CORE_SANITIZE)
        add_test(NAME soak_memory COMMAND ${CMAKE_COMMAND} -E env CORE_SOAK_SECONDS=30 $<TARGET_FILE:bench_soak_memory>)
        set_tests_properties(soak_memory PROPERTIES LABELS soak TIMEOUT 120)
    endif()
endif()
