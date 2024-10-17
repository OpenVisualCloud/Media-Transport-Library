#!/bin/bash

echo "Downloading fff.library"
wget https://raw.githubusercontent.com/meekrosoft/fff/refs/heads/master/fff.h -O ./tests/fff.h

echo "Compiling the library"
meson build && cd build

echo "Running the tests"
meson test

