// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2024 Intel Corporation

// FFF requires DEFINE_FFF_GLOBALS to live in exactly one translation unit
// per binary. Linking this static lib into every unit binary satisfies that.

#include "../fff.h"

DEFINE_FFF_GLOBALS;
