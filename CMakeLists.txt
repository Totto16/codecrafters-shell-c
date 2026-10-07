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

find_package(PkgConfig REQUIRED)
pkg_check_modules(libedit REQUIRED IMPORTED_TARGET libedit)
target_link_libraries(shell PRIVATE PkgConfig::libedit)

find_package(tstr CONFIG REQUIRED)
target_link_libraries(shell PRIVATE tstr)

find_package(tvec CONFIG REQUIRED)
target_link_libraries(shell PRIVATE tvec)
