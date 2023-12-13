// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2023 Intel Corporation

// Usage: swig -python -I/usr/local/include -o pymtl_wrap.c pymtl.i

%module pymtl

%include <stdint.i>

%begin %{
#define SWIG_PYTHON_CAST_MODE
%}

%{
#include <mtl/mtl_api.h>
%}

%define __MTL_LIB_BUILD__
%enddef

%include <mtl/mtl_api.h>