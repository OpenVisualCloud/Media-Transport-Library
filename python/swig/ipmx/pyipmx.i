// SPDX-License-Identifier: LicenseRef-IPMX-TBD

// Usage: swig -python -I/usr/local/include -o pyipmx_wrap.c pyipmx.i

%module pyipmx

// The handle types come from pymtl, so its handles pass into these functions.
%import "../pymtl.i"

%{
#include <mtl/ipmx/ipmx_api.h>
%}

// get_sdp(handle) returns [ret, sdp]; the zeroed buffer reads "" when the library leaves it untouched.
%typemap(in, numinputs=0) (char* buf, size_t len) {
  $1 = calloc(IPMX_SDP_MAX, 1);
  if (!$1) SWIG_exception_fail(SWIG_MemoryError, "no memory for the SDP buffer");
  $2 = IPMX_SDP_MAX;
}
%typemap(argout, fragment="SWIG_FromCharPtr") (char* buf, size_t len) {
  %append_output(SWIG_FromCharPtr($1));
}
%typemap(freearg) (char* buf, size_t len) {
  free($1);
}

%include <mtl/ipmx/ipmx_api.h>
