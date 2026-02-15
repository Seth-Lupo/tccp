#!/bin/bash
set -e

cd "$(dirname "$0")/.."

cmake -B build -DTCCP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
