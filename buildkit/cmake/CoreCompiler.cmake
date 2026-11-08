# core_target_defaults(<target> [SHADOW_LOCAL]): the warning set every component builds with. SHADOW_LOCAL narrows GCC's
# -Wshadow to locals, for code whose scoped enums share names with types.
include_guard(GLOBAL)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(core_target_defaults target)
    cmake_parse_arguments(ARG "SHADOW_LOCAL" "" "" ${ARGN})
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(ARG_SHADOW_LOCAL AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -Wshadow=local)
        else()
            target_compile_options(${target} PRIVATE -Wshadow)
        endif()
    elseif(MSVC)
        # C4324: structures are padded by alignas on purpose, to keep hot counters on their own cache lines.
        target_compile_options(${target} PRIVATE /W4 /permissive- /wd4324)
        # C4996: getenv and fopen are standard C, used where their results are checked.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()
