# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Negative checks of the VSF report parser that decides every IPMX verdict.

The VSF pcap validators exit 0 whatever they find, so a parser that misses a
FAIL line would let every IPMX case pass. These cases feed it report text in
the validators' own layout (``SHALL requirements`` / ``SHOULD requirements``
headers, then one ``PASS|FAIL|CANNOT_TEST <id>: <text>`` line per
requirement) and need no capture or VSF clone.
"""

import pytest
from tests.single.ipmx import ipmx_vsf

_REPORT = """
Initial block version (first Sender Report in PCAP): 2
SHALL requirements
49/50 passed, 1 failed, 1 cannot test
PASS TR-10-1-SR-PRESENT: IPMX Senders SHALL send RTCP Sender Reports (TR-10-1 §8.7).
FAIL TR-10-1-SR-IPMX: RTCP Sender Reports SHALL include an IPMX Info Block (TR-10-1 §8.7).
CANNOT_TEST RAW-CLI-PORT: Operator-supplied --port SHALL match the selected RTP destination port.
SHOULD requirements
6/7 passed, 1 failed
PASS TR-10-2-7b: UDP destination port SHOULD be > 5000 (TR-10-2 §7).
FAIL TR-10-1-8.7-RC: RTCP SR reception report count (RC) should be 0 (TR-10-1 §8.7).
"""


@pytest.mark.nightly
def test_ipmx_vsf_report_levels():
    """A FAIL lands under the level of its header, not of the words in its text.

    TR-10-1-8.7-RC is a SHOULD whose text says "should"; a SHALL FAIL must
    never be dropped, and CANNOT_TEST and the first-SR block version are kept.
    """
    report = ipmx_vsf.parse_report(_REPORT)
    assert report.passed == 2
    assert [i.split(":")[0] for i in report.shall_fail] == ["TR-10-1-SR-IPMX"]
    assert [i.split(":")[0] for i in report.should_fail] == ["TR-10-1-8.7-RC"]
    assert report.cannot_test == ["RAW-CLI-PORT"]
    assert report.block_version == 2


@pytest.mark.nightly
def test_ipmx_vsf_report_untestable():
    """A SHALL the capture should allow but the validator could not test is caught.

    TR-10-9 11.2a needs 2 s of frames; CANNOT_TEST for it means a short capture.
    """
    report = ipmx_vsf.parse_report(
        "SHALL requirements\n"
        "CANNOT_TEST TR-10-9-11.2a: First-packet capture times SHALL have max-min "
        "variation <= 2ms over any 2s window (TR-10-9 §11.2).\n"
    )
    assert set(report.cannot_test) - ipmx_vsf.UNTESTABLE["raw"] == {"TR-10-9-11.2a"}


@pytest.mark.nightly
def test_ipmx_vsf_frames_cover_2s():
    """The validated span holds 2 s of frames at every MVP rate (TR-10-9 11.2)."""
    tools = {"max_frames": 0}
    for rate, fps in (("50", 50), ("60000/1001", 60000 / 1001), ("60", 60)):
        assert (ipmx_vsf.validated_frames(tools, rate) - 1) / fps > 2.0
