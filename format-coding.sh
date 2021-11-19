#!/bin/bash

set -e

find . -regex '.*\.\(cpp\|hpp\|cu\|c\|h\)' -exec clang-format -i {} \;
