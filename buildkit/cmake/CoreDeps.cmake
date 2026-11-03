# core_require(<name> <tag>): make core::<name> available.
#
# While the components live together in libcore this is only needed by repositories outside it. It uses a
# sibling checkout when -DDEVELOP_LOCAL_ROOT=<dir> points at one, otherwise fetches the pinned tag.
include_guard(GLOBAL)
include(FetchContent)

function(core_require name tag)
    if(TARGET core::${name})
        return()
    endif()
    if(DEFINED CORE_LOCAL_ROOT AND EXISTS "${CORE_LOCAL_ROOT}/${name}/CMakeLists.txt")
        add_subdirectory("${CORE_LOCAL_ROOT}/${name}" "${CMAKE_BINARY_DIR}/_deps/${name}-build")
    else()
        FetchContent_Declare(${name}
            GIT_REPOSITORY https://github.com/philipp-orlov/${name}.git
            GIT_TAG        ${tag}
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(${name})
    endif()
endfunction()
