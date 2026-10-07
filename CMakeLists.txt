cmake_minimum_required(VERSION 3.13)

project(codecrafters-shell)

file(GLOB_RECURSE SOURCE_FILES src/*.c src/*.h)

set(CMAKE_C_STANDARD 23) # Enable the C23 standard

add_executable(shell ${SOURCE_FILES})

target_compile_options(shell PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

set(USE_EDIT_LIBRARY "readline"
    CACHE STRING "Line editing backend")
set_property(CACHE USE_EDIT_LIBRARY PROPERTY STRINGS readline libedit)

if(USE_EDIT_LIBRARY STREQUAL "libedit")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(libedit REQUIRED IMPORTED_TARGET libedit)
    target_link_libraries(shell PRIVATE PkgConfig::libedit)

    target_compile_definitions(shell PRIVATE SHELL_USE_EDIT_LIBRARY=0)

elseif(USE_EDIT_LIBRARY STREQUAL "readline")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(readline REQUIRED IMPORTED_TARGET readline)
    target_link_libraries(shell PRIVATE PkgConfig::readline)
    target_compile_definitions(shell PRIVATE SHELL_USE_EDIT_LIBRARY=1)
else()
    message(FATAL_ERROR
        "Invalid USE_EDIT_LIBRARY: ${USE_EDIT_LIBRARY}")
endif()

find_package(tstr CONFIG REQUIRED)
target_link_libraries(shell PRIVATE tstr)

find_package(tvec CONFIG REQUIRED)
target_link_libraries(shell PRIVATE tvec)
