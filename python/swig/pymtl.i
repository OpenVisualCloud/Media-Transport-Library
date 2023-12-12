// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2023 Intel Corporation

// Usage: swig -python -I/usr/local/include mtl.i

%module pymtl

%{
#include "mtl/mtl_api.h"
%}

%define __MTL_LIB_BUILD__
%enddef

%include "mtl/mtl_api.h"