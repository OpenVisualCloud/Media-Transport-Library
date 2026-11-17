# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# The warning and hardening options of the MTL meson projects, see
# doc/build.md, for the programs of the tools that build with make. The
# caller's CFLAGS, CPPFLAGS and LDFLAGS come after them, so they can change
# any of them: make CFLAGS=-Wno-error makes the warnings warnings again,
# except format-security, and make CFLAGS='-O0 -g' a debug build, which
# gets no _FORTIFY_SOURCE.

TOOLS_CFLAGS := -O2 -Wall -Wextra -Werror -Wformat -Wformat-security -Werror=format-security \
	-Wimplicit-fallthrough -fstack-protector-strong -fstack-clash-protection \
	-fcf-protection=full -fPIE
TOOLS_LDFLAGS := -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack

# _FORTIFY_SOURCE at the level that the toolchain applies, as the meson
# projects probe it: 3, or 2 with gcc 11, where glibc warns that it treats 3
# like 2, which -Werror turns into an error. None when CFLAGS or CPPFLAGS set a
# level or a sanitizer, or when their last -O is -O0.
tools_flags := $(CFLAGS) $(CPPFLAGS)
ifeq ($(findstring _FORTIFY_SOURCE,$(tools_flags))$(filter -fsanitize=%,$(tools_flags)),)
ifneq ($(lastword $(filter -O%,$(tools_flags))),-O0)
tools_defines := $(shell gcc $(tools_flags) -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 \
	-include string.h -dM -E -x c /dev/null 2>/dev/null)
TOOLS_CFLAGS += -U_FORTIFY_SOURCE \
	-D_FORTIFY_SOURCE=$(if $(findstring __USE_FORTIFY_LEVEL 3,$(tools_defines)),3,2)
endif
endif

# As make's LINK.c, for a program built from its C sources in one step, with
# gcc, which the Makefiles always used
TOOLS_LINK = gcc $(TOOLS_CFLAGS) $(CFLAGS) $(CPPFLAGS) $(TOOLS_LDFLAGS) $(LDFLAGS)
