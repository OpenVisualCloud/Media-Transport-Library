#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Print the compiler hardening flags of the MTL meson projects, see doc/build.md.

Usage: hardening_flags.py LANGUAGE OPTIMIZATION SANITIZE COMPILER [ARG...]

LANGUAGE is c or c++, OPTIMIZATION and SANITIZE are the meson options
optimization and b_sanitize, COMPILER and ARG are the compiler command and the
c_args or cpp_args of the project. Prints the compile flags on the first line
and the link flags on the second.
"""

import os
import subprocess  # nosec B404 # runs the compiler meson gives, as an argv list
import sys
import tempfile

if len(sys.argv) < 5:
    sys.exit(__doc__)
language, optimization, sanitize, *compiler = sys.argv[1:]


def run(*args, source=""):
    return subprocess.run(  # nosec B603 # the compiler of the meson project
        compiler + list(args) + ["-x", language, "-"],
        input=source,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        universal_newlines=True,
    )


# A flag the compiler doesn't support is dropped, as get_supported_arguments().
with tempfile.TemporaryDirectory() as tmp:
    probe = ["-Werror", "-c", "-o", os.path.join(tmp, "probe.o")]
    flags = [
        flag
        for flag in (
            "-fstack-protector-strong",
            "-fstack-clash-protection",
            "-fcf-protection=full",
        )
        if run(flag, *probe, source="int main(void) { return 0; }").returncode == 0
    ]
flags += [
    "-Wformat",
    "-Wformat-security",
    "-Werror=format-security",
    "-Wimplicit-fallthrough",
]
# Bounds and precondition checks in libstdc++ (libc++ ignores it).
if language == "c++":
    flags.append("-D_GLIBCXX_ASSERTIONS")
# glibc needs optimization for it, and the sanitizers don't support it. A level
# the compiler (Ubuntu gcc) or ARG sets is kept. meson's own checks can't see
# it: they don't optimize, and meson 1.11 and later add -U_FORTIFY_SOURCE.
if optimization not in ("0", "plain") and sanitize == "none":
    if "#define _FORTIFY_SOURCE " not in run("-O2", "-dM", "-E").stdout:
        # 3 where the compiler and glibc support it (gcc 12+, clang 9+, glibc 2.34+)
        level = run("-O2", "-D_FORTIFY_SOURCE=3", "-include", "string.h", "-dM", "-E")
        level3 = "#define __USE_FORTIFY_LEVEL 3" in level.stdout
        flags.append("-D_FORTIFY_SOURCE=3" if level3 else "-D_FORTIFY_SOURCE=2")
print(" ".join(flags))
print("-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack")
