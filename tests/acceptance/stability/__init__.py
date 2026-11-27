# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""MTL stability engine.

Runs a long, mixed MTL workload (every session type, the pipeline and the
frame/RTP APIs, user pacing, internal and external converters, RxTxApp,
FFmpeg and GStreamer) on fenced CPUs, scans the logs of each process while it
runs, and sends one session after the other to EBU LIST for a compliance
verdict. ``python -m stability --help`` shows the commands; README.md the rules.
"""
