add_library(develop_bench STATIC soak/soak.cpp)
add_library(core::bench ALIAS develop_bench)
target_include_directories(develop_bench PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(develop_bench PUBLIC cxx_std_17)
target_link_libraries(develop_bench PUBLIC core::sys core::base)
develop_target_defaults(develop_bench)

if(DEVELOP_BUILD_TESTS)
    add_executable(bench_soak_memory soak/soak_memory.cpp)
    target_link_libraries(bench_soak_memory PRIVATE core::bench core::mem core::sched core::log)
    develop_target_defaults(bench_soak_memory)
    # Sanitizers slow the size drift of this workload so much that it never reaches a steady state in a short run, so
    # the soak is registered for unsanitized builds only. 12 seconds by default; set DEVELOP_SOAK_SECONDS=86400 for a
    # real one.
    if(NOT DEVELOP_SANITIZE)
        add_test(NAME soak_memory COMMAND ${CMAKE_COMMAND} -E env DEVELOP_SOAK_SECONDS=12 $<TARGET_FILE:bench_soak_memory>)
        set_tests_properties(soak_memory PROPERTIES LABELS soak TIMEOUT 120)
    endif()
endif()
