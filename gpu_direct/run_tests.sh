#!/bin/bash

echo "Downloading fff.library"
wget https://raw.githubusercontent.com/meekrosoft/fff/5111c61e1ef7848e3afd3550044a8cf4405f4199/fff.h -O ./tests/fff.h

echo "Compiling the library"
meson setup build -Denable_tests=true && cd build || exit

echo "Running the tests"
meson test
