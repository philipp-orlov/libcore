# develop_require(<name> <tag>): make develop::<name> available.
#
# While the components live together in develop-core this is only needed by repositories outside it. It uses a
# sibling checkout when -DDEVELOP_LOCAL_ROOT=<dir> points at one, otherwise fetches the pinned tag.
include_guard(GLOBAL)
include(FetchContent)

function(develop_require name tag)
    if(TARGET develop::${name})
        return()
    endif()
    if(DEFINED DEVELOP_LOCAL_ROOT AND EXISTS "${DEVELOP_LOCAL_ROOT}/${name}/CMakeLists.txt")
        add_subdirectory("${DEVELOP_LOCAL_ROOT}/${name}" "${CMAKE_BINARY_DIR}/_deps/${name}-build")
    else()
        FetchContent_Declare(${name}
            GIT_REPOSITORY https://github.com/philipp-orlov/${name}.git
            GIT_TAG        ${tag}
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(${name})
    endif()
endfunction()
