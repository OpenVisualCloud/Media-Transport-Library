# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Real PTP conformance checks.

Unlike test_st20_interfaces_mix_refactored (which only passes ``--ptp`` and
checks video integrity), these tests assert the PTP behavior itself:

* ``test_mtl_internal_ptp_converges`` -- MTL's own software PTP client
  (``mt_ptp.c``) actually reaches the "connected" state and its reported
  offset from the grandmaster is within tolerance. Single host, no external
  daemon required.
* ``test_mtl_and_ptp4l_agree_on_grandmaster`` -- external-GM interoperability:
  runs ``ptp4l`` (via the shared ``ptp_sync`` fixture) as an independent PTP
  client on the same PF/wire MTL's VF sits on, and asserts *both* clients
  converge concurrently -- i.e. MTL's software PTP client and a standard
  Linux PTP stack agree the link has a working, lockable grandmaster.
"""
import logging

import pytest
from mtl_engine.media_files import yuv_files_422rfc10

from ..ptp_helpers import assert_mtl_ptp_converged, assert_ptp4l_converged

logger = logging.getLogger(__name__)


def _run_short_ptp_session(
    app_factory, setup_interfaces, mtl_path, host, test_time, media_file
):
    """Smallest possible st20p session with PTP enabled; returns the app."""
    interfaces_list = setup_interfaces.get_interfaces_list_single("VF")
    media_file_info, media_file_path = media_file
    app = app_factory("rxtxapp")
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        enable_ptp=True,
        test_time=test_time,
    )
    app.execute_test(build=mtl_path, test_time=test_time, host=host)
    return app


@pytest.mark.nightly
@pytest.mark.ptp
@pytest.mark.refactored
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["Crosswalk_720p"]],
    indirect=["media_file"],
    ids=["Crosswalk_720p"],
)
def test_mtl_internal_ptp_converges(
    hosts,
    mtl_path,
    setup_interfaces,
    test_time,
    app_factory,
    media_file,
    require_grandmaster,
):
    """MTL's internal PTP client (``--ptp``) must actually lock, not free-run.

    Regression target: a test marked ``@pytest.mark.ptp`` that never asserts
    on PTP state passes even with no reachable grandmaster. The
    ``require_grandmaster`` fixture already confirmed a grandmaster is
    reachable (erroring out fast if not, before this body even runs) -- so
    any assertion failure here is a genuine MTL-side PTP regression.
    """
    host = list(hosts.values())[0]
    app = _run_short_ptp_session(
        app_factory, setup_interfaces, mtl_path, host, test_time, media_file
    )
    assert_mtl_ptp_converged(app.last_output, port=0)


@pytest.mark.nightly
@pytest.mark.ptp
@pytest.mark.refactored
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["Crosswalk_720p"]],
    indirect=["media_file"],
    ids=["Crosswalk_720p"],
)
def test_mtl_and_ptp4l_agree_on_grandmaster(
    hosts,
    mtl_path,
    setup_interfaces,
    test_time,
    ptp_sync,
    app_factory,
    media_file,
    require_grandmaster,
):
    """External-GM interoperability: MTL's PTP client and ptp4l agree.

    ``ptp_sync`` starts ``ptp4l`` (an independent, standard Linux PTP client)
    on the PF whose VFs MTL uses for this session, picking the interface via
    its own resolution logic (explicit ``sniff_interface``/``sniff_pci_device``
    config, or an automatic topology-based fallback -- either way it writes
    its log to ``/tmp/ptp4l-<iface>.log``). Both ptp4l and MTL are passive
    BMCA slaves observing the same Announce/Sync stream, so they don't fight
    over the PHC (unlike ptp4l+phc2sys) and can run concurrently.

    ``require_grandmaster`` already confirmed ptp4l heard a foreign master
    (erroring out fast if not) and returns its log path -- so any assertion
    failure below is a genuine MTL-side or ptp4l-side regression, not a
    missing-grandmaster environment issue.
    """
    host = list(hosts.values())[0]
    app = _run_short_ptp_session(
        app_factory, setup_interfaces, mtl_path, host, test_time, media_file
    )
    ptp4l_log = (
        host.connection.execute_command(
            f"cat {require_grandmaster}", expected_return_codes=None
        ).stdout
        or ""
    )
    assert_mtl_ptp_converged(app.last_output, port=0)
    assert_ptp4l_converged(ptp4l_log)
