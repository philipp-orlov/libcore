add_library(develop_json STATIC src/json.cpp src/writer.cpp)
add_library(core::json ALIAS develop_json)
target_include_directories(develop_json PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(develop_json PUBLIC cxx_std_17)
target_link_libraries(develop_json PUBLIC core::base)
develop_target_defaults(develop_json)

if(DEVELOP_BUILD_TESTS)
    add_executable(json_test_json tests/test_json.cpp)
    target_link_libraries(json_test_json PRIVATE core::json)
    add_test(NAME json_json COMMAND json_test_json)

    add_executable(json_test_writer tests/test_writer.cpp)
    target_link_libraries(json_test_writer PRIVATE core::json)
    target_include_directories(json_test_writer PRIVATE ${CMAKE_SOURCE_DIR}/libbase/tests ${CMAKE_SOURCE_DIR}/libbench/include)
    develop_target_defaults(json_test_writer)
    add_test(NAME json_writer COMMAND json_test_writer)

    add_executable(json_test_soak tests/test_json_soak.cpp)
    target_link_libraries(json_test_soak PRIVATE core::json)
    add_test(NAME json_soak COMMAND json_test_soak)
    set_tests_properties(json_soak PROPERTIES TIMEOUT 300 LABELS soak)

    # The same parser on the snprintf/strtod path of a standard library without floating-point <charconv>
    # (libstdc++ before 11, e.g. GCC 9 on JetPack 5).
    add_executable(json_test_fallback tests/test_json.cpp src/json.cpp)
    target_include_directories(json_test_fallback PRIVATE include)
    target_link_libraries(json_test_fallback PRIVATE core::base)
    target_compile_features(json_test_fallback PRIVATE cxx_std_17)
    target_compile_definitions(json_test_fallback PRIVATE JSON_FP_CHARCONV=0)
    add_test(NAME json_fallback COMMAND json_test_fallback)

    add_executable(json_fuzz fuzz/fuzz_json.cpp fuzz/fuzz_driver.cpp)
    target_link_libraries(json_fuzz PRIVATE core::json)
    add_test(NAME json_fuzz_smoke COMMAND json_fuzz --iterations=20000 --seed=1)
endif()
