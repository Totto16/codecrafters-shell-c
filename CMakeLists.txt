cmake_minimum_required(VERSION 3.13)

project(codecrafters-shell)

file(GLOB_RECURSE SOURCE_FILES src/*.c src/*.h)

set(CMAKE_C_STANDARD 23) # Enable the C23 standard

add_executable(shell ${SOURCE_FILES})

function(add_supported_c_flags target)
    include(CheckCCompilerFlag)

    foreach(flag IN LISTS ARGN)
        string(MAKE_C_IDENTIFIER "${flag}" flag_id)
        set(check_var "HAVE_C_FLAG_${flag_id}")

        check_c_compiler_flag("${flag}" "${check_var}")

        if(${check_var})
            target_compile_options("${target}" PRIVATE "${flag}")
        endif()
    endforeach()
endfunction()

set(C_WARNING_FLAGS
    -Wassign-enum
    -Wenum-conversion
    -Wenum-switch
    -fstrict-enums
    -Wenum-identifier
    -Wenum-compare
    -Wenum-enum-conversion
    -Wenum-float-conversion
    -Wenum-too-large
    -Wenum-compare-switch
    -Wenum-compare-conditional
    -fno-strict-aliasing
    -Wpointer-arith
    -Wmissing-declarations
    -Wformat=2
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wnested-externs
    -Wold-style-definition
    -Wundef
    -Wunused
    -Wcast-align
    -Wmissing-noreturn
    -Wmissing-format-attribute
    -Wmissing-include-dirs
    -Wlogical-op
    -Wignored-qualifiers
    -Werror=redundant-decls
    -Werror=implicit
    -Werror=nonnull
    -Werror=init-self
    -Werror=main
    -Werror=missing-braces
    -Werror=sequence-point
    -Werror=return-type
    -Werror=trigraphs
    -Werror=array-bounds
    -Werror=write-strings
    -Werror=address
    -Werror=int-to-pointer-cast
    -Werror=pointer-to-int-cast
    -Werror=empty-body
    -Werror=strict-aliasing
    -Wsign-compare
    -Wno-cast-function-type
    -Wunused-parameter
    -Wmissing-field-initializers
    -Wtype-limits
    -Wshadow
    -Wfloat-conversion
    -Wno-nullability-extension
    -Wno-nullability-completeness
    -Wcast-qual
    -Wwrite-strings
    -Wdiscarded-qualifiers
    -Wconversion
    -Wsign-conversion
    -Waddress
    -Wfloat-equal
)

target_compile_options(shell PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_supported_c_flags(shell ${C_WARNING_FLAGS})

set(USE_EDIT_LIBRARY "readline"
    CACHE STRING "Line editing backend")
set_property(CACHE USE_EDIT_LIBRARY PROPERTY STRINGS readline libedit custom)

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
elseif(USE_EDIT_LIBRARY STREQUAL "custom")
    target_compile_definitions(shell PRIVATE SHELL_USE_EDIT_LIBRARY=2)
else()
    message(FATAL_ERROR
        "Invalid USE_EDIT_LIBRARY: ${USE_EDIT_LIBRARY}")
endif()

find_package(tstr CONFIG REQUIRED)
target_link_libraries(shell PRIVATE tstr)

find_package(tvec CONFIG REQUIRED)
target_link_libraries(shell PRIVATE tvec)

find_package(trtti CONFIG REQUIRED)
target_link_libraries(shell PRIVATE trtti)

find_package(tmap CONFIG REQUIRED)
target_link_libraries(shell PRIVATE tmap)
