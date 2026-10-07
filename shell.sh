#!/bin/sh
#
# Use this script to run your program LOCALLY.
#
# Note: Changing this script WILL NOT affect how CodeCrafters runs your program.
#
# Learn more: https://codecrafters.io/program-interface

set -e # Exit early if any commands fail

BUILD_TYPE="Debug"

if [ "$USE_GCC" = "1" ]; then
  export CC=gcc-15
  export CXX=g++-15
  BUILD_DIR="./build_gcc"
else
  BUILD_DIR="./build"
fi

# Copied from .codecrafters/compile.sh
#
# - Edit this to change how your program compiles locally
# - Edit .codecrafters/compile.sh to change how your program compiles remotely
(
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  cmake -B "$BUILD_DIR" -S . -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "-DCMAKE_BUILD_TYPE=$BUILD_TYPE" -DUSE_EDIT_LIBRARY="readline"
  cmake --build "$BUILD_DIR"
  echo "*" >"$BUILD_DIR/.gitignore"
)

(
  cd "$(dirname "$0")" # Ensure compile steps are run within the repository directory
  cmake -B "${BUILD_DIR}_el" -S . -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "-DCMAKE_BUILD_TYPE=$BUILD_TYPE" -DVCPKG_MANIFEST_FEATURES=libedit -DUSE_EDIT_LIBRARY="libedit"
  cmake --build "${BUILD_DIR}_el"
  echo "*" >"${BUILD_DIR}_el/.gitignore"
)

# Copied from .codecrafters/run.sh
#
# - Edit this to change how your program runs locally
# - Edit .codecrafters/run.sh to change how your program runs remotely

SHELL="$(realpath "$(dirname "$0")/$BUILD_DIR/shell")"

exec "$SHELL" "$@"
