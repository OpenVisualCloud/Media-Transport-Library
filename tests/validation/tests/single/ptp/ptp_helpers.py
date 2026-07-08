# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Shared assertion helpers for PTP conformance tests.

Parses the periodic ``PTP(<port>): ...`` stat lines MTL's own software PTP
client (``mt_ptp.c::ptp_stat``) prints to RxTxApp's stdout, and the ``rms``
lines ``ptp4l`` prints to its own log, so tests can assert real
synchronization happened instead of only checking the app didn't crash.
"""
import re
import time

# mt_ptp.c auto-tunes its own error tolerance to ~2x the running average
# (min 100us); 1ms is far looser than that internal threshold, so this only
# catches a genuinely unsynced/free-running clock, not measurement noise.
MTL_PTP_DELTA_TOLERANCE_NS = 1_000_000  # 1ms
PTP4L_RMS_TOLERANCE_NS = 1_000  # ptp4l settles to single/double-digit ns on a quiet LAN

_PTP_DELTA_RE = re.compile(
    r"PTP\((\d+)\): delta avg (-?\d+), min (-?\d+), max (-?\d+), cnt (\d+)"
)
_PTP4L_RMS_RE = re.compile(r"rms\s+(\d+)\s+max\s+(\d+)")
_PTP4L_FOREIGN_MASTER_RE = re.compile(r"new foreign master")


def wait_for_ptp4l_foreign_master(
    host, log_path: str, timeout_s: float = 15.0, poll_s: float = 1.0
) -> None:
    """Fail fast if ptp4l never hears a foreign master on *log_path*.

    A reachable external PTP grandmaster is an environment precondition,
    not a per-test assertion -- so this raises ``RuntimeError`` (never
    ``pytest.skip``) on timeout. Skip is reserved for genuinely unsupported
    configurations; "no grandmaster reachable" means the test fabric is not
    ready, which must surface as a visible failure so it gets fixed, not a
    silently-passing skip. Runs BEFORE the expensive RxTxApp session so a
    dead fabric fails in ~15s instead of after a multi-minute run.
    """
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        text = (
            host.connection.execute_command(
                f"cat {log_path} 2>/dev/null", expected_return_codes=None
            ).stdout
            or ""
        )
        if _PTP4L_FOREIGN_MASTER_RE.search(text) or _PTP4L_RMS_RE.search(text):
            return
        time.sleep(poll_s)
    raise RuntimeError(
        f"ENVIRONMENT NOT READY: ptp4l on {host.name} never heard a foreign "
        f"master within {timeout_s}s (log={log_path}). No PTP grandmaster is "
        "reachable on this test fabric -- fix the network/grandmaster before "
        "re-running; this is not a code defect."
    )


def parse_mtl_ptp_delta_samples(app_output: str, port: int = 0):
    """Return ``[(avg, min, max, cnt), ...]`` for ``PTP(port)`` stat samples.

    An empty list means the app never printed a delta sample for *port* --
    i.e. its internal PTP client never left the "not connected" state (most
    commonly: no reachable grandmaster on this test fabric).
    """
    return [
        (int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))
        for m in _PTP_DELTA_RE.finditer(app_output)
        if int(m.group(1)) == port
    ]


def mtl_ptp_connected(app_output: str, port: int = 0) -> bool:
    """True if MTL's internal PTP client on *port* ever left 'not connected'.

    Only meaningful once a grandmaster's reachability has already been
    confirmed (see :func:`wait_for_ptp4l_foreign_master`) -- at that point
    ``False`` means an MTL-side regression, not a missing grandmaster.
    """
    return bool(parse_mtl_ptp_delta_samples(app_output, port))


def assert_mtl_ptp_converged(
    app_output: str, port: int = 0, tolerance_ns: int = MTL_PTP_DELTA_TOLERANCE_NS
) -> None:
    """Assert MTL's own internal PTP client (``--ptp``) synced within tolerance.

    Precondition: grandmaster reachability already confirmed (see
    :func:`wait_for_ptp4l_foreign_master`) -- so "never connected" here is a
    genuine MTL-side regression, not a missing-grandmaster environment issue.
    """
    samples = parse_mtl_ptp_delta_samples(app_output, port)
    assert samples, (
        f"MTL internal PTP client on port {port} never reported a delta "
        f"sample (stayed in 'not connected' state) -- look for "
        f"'PTP({port}): not connected' in the app output"
    )
    avg, _min, _max, cnt = samples[-1]
    assert abs(avg) <= tolerance_ns, (
        f"MTL internal PTP client on port {port} did not converge: last "
        f"delta avg={avg}ns exceeds tolerance {tolerance_ns}ns (cnt={cnt})"
    )


def parse_ptp4l_rms_samples(ptp4l_log_text: str):
    """Return ``[(rms_ns, max_ns), ...]`` parsed from a ``ptp4l -m`` log."""
    return [
        (int(m.group(1)), int(m.group(2)))
        for m in _PTP4L_RMS_RE.finditer(ptp4l_log_text)
    ]


def ptp4l_connected(ptp4l_log_text: str) -> bool:
    """True if ptp4l ever printed an 'rms' sample (reached SLAVE state).

    Only meaningful once grandmaster reachability has already been
    confirmed (see :func:`wait_for_ptp4l_foreign_master`) -- at that point
    ``False`` means ptp4l lost lock after acquiring it, a real regression.
    """
    return bool(parse_ptp4l_rms_samples(ptp4l_log_text))


def assert_ptp4l_converged(
    ptp4l_log_text: str, tolerance_ns: int = PTP4L_RMS_TOLERANCE_NS
) -> None:
    """Assert ptp4l (the external-GM reference client) converged within tolerance.

    Precondition: call :func:`ptp4l_connected` first and skip the test if
    it's False -- this function assumes SLAVE state was reached.
    """
    samples = parse_ptp4l_rms_samples(ptp4l_log_text)
    assert (
        samples
    ), "ptp4l never printed an 'rms' sample -- it never reached SLAVE state"
    last_rms, _last_max = samples[-1]
    assert (
        last_rms <= tolerance_ns
    ), f"ptp4l did not converge: last rms={last_rms}ns exceeds tolerance {tolerance_ns}ns"
