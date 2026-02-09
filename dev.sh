#!/bin/bash
cd "$(dirname "$0")/build"

cmake_args=".."
if [ ! -f CMakeCache.txt ]; then
    cmake_args=".. -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake"
fi

cmake $cmake_args && make -j4
