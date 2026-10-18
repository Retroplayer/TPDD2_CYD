#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
c++ -std=c++17 -Wall -Wextra -Werror -g -O1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I lib/Tpdd lib/Tpdd/Tpdd.cpp tests/core_test.cpp -o build/core-test
./build/core-test
