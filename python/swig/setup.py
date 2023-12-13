# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

from setuptools import Extension, setup

mtl_module = Extension(
    "_pymtl",
    sources=["pymtl_wrap.c"],
    # include_dirs=['/path/to/include'],
    # library_dirs=['/path/to/lib'],
    libraries=["mtl"],
)

setup(
    name="pymtl",
    version="0.1",
    ext_modules=[mtl_module],
)
