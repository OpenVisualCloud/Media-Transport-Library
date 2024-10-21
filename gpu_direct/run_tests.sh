#!/bin/bash

echo "Downloading fff.library"
wget https://raw.githubusercontent.com/meekrosoft/fff/refs/heads/master/fff.h -O ./tests/fff.h

echo "Compiling the library"
meson setup build -Denable_tests=true && cd build || exit

echo "Running the tests"
meson test
