# SPDX-License-Identifier: LicenseRef-IPMX-TBD

"""Module of MTL IPMX python binding setup."""

from setuptools import Extension, setup

setup(
    name="pyipmx",
    version="0.1",
    description="Python bindings for the IPMX API of libmtl using SWIG",
    ext_modules=[Extension("_pyipmx", sources=["pyipmx_wrap.c"], libraries=["mtl"])],
    py_modules=["pyipmx"],
)
