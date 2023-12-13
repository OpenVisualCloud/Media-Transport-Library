# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

from setuptools import Extension, setup

pymtl_module = Extension(
    "_pymtl",
    sources=["pymtl_wrap.c"],
    # include_dirs=['/path/to/include'],
    # library_dirs=['/path/to/lib'],
    libraries=["mtl"],
)

setup(
    name="pymtl",
    version="0.1",
    description="Python bindings for libmtl using SWIG",
    ext_modules=[pymtl_module],
    py_modules=["pymtl"],
)
