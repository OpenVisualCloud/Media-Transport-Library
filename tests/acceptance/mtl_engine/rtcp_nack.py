# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Crafted RTCP NACK datagrams for the TX retransmit path.

A TX video session with RTCP enabled receives NACKs on UDP port
``udp_port + 1`` and parses them in ``mt_rtcp_tx_parse_rtcp_packet()``.
:func:`start_rtcp_nack_injector` sends a fixed batch of malformed and
hostile NACKs to that port from the test host, again and again, while the
session runs. The batch holds each attack of
``tests/unit/session/rtcp/nack_attack_test.cpp``.
"""

import base64
import struct

from .execute import run

RTCP_FLAGS = 0x80
RTCP_PTYPE_NACK = 204
RTCP_NAME = b"IMTL"
MAX_FCIS = 256  # MT_RTCP_MAX_FCIS


def _nack(
    fcis, len_field=None, flags=RTCP_FLAGS, ptype=RTCP_PTYPE_NACK, name=None, ssrc=0
):
    """Build one IMTL NACK. ``len_field`` defaults to the RFC 3550 value.

    ``ssrc`` is the RFC4585 "SSRC of media source". It is 0 for the crafted
    batch, which does not know the session ssrc. A test that drives the accept
    path passes the real session ssrc.
    """
    if len_field is None:
        len_field = 2 + len(fcis)
    hdr = struct.pack("!BBHI4s", flags, ptype, len_field, ssrc, name or RTCP_NAME)
    return hdr + b"".join(struct.pack("!HH", s, f) for s, f in fcis)


def well_formed_nacks(ssrc: int):
    """Return valid single-packet NACKs that carry ``ssrc``.

    Every NACK passes each header guard and the RFC4585 ssrc check when the
    check is on and ``ssrc`` is the session ssrc. Each asks for one packet at
    a spread of sequence numbers, so a NACK hits a buffered packet whatever
    the current ring holds. The TX must count each as received.
    """
    return [_nack([(seq, 0)], ssrc=ssrc) for seq in range(0, 1024, 128)]


# The first four pass the header checks, the next seven are "nack drop invalid".
# The three bad-len NACKs also count in "nack recv", and a kernel socket does
# not give the empty datagram to the parser.
CRAFTED_NACKS = [
    _nack([(0, 0xFFFE)]),  # follow past the ring
    _nack([(0, 0xFFFF)]),  # bulk 65536 must not wrap to 0
    _nack([(0, 1023)] * MAX_FCIS),  # 256 x full-ring repeat
    _nack([(0, 0)] * (MAX_FCIS + 44)),  # fci count over the cap
    _nack([], len_field=0),  # num_fcis underflow
    _nack([], len_field=1),  # num_fcis underflow
    _nack([(0, 0)], len_field=2 + 4),  # len past the received bytes
    _nack([(0, 0)], flags=0x81),  # wrong flags
    _nack([(0, 0)], name=b"XXXX"),  # not IMTL
    _nack([])[:4],  # runt shorter than the header
    b"",  # empty datagram
    _nack([], len_field=0xFFFF, ptype=200),  # not a NACK
]

_SENDER = """
import socket, time
pkts = [bytes.fromhex(h) for h in {hexes!r}]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
end = time.monotonic() + {duration}
batches = 0
while time.monotonic() < end:
    for p in pkts:
        try:
            s.sendto(p, ({ip!r}, {port}))
        except OSError:
            pass
    batches += 1
    time.sleep({interval})
print("rtcp nack injector batches", batches)
"""


def start_rtcp_nack_injector(
    host, dst_ip: str, dst_port: int, duration: int, interval: float = 0.2, packets=None
):
    """Send NACK datagrams to ``dst_ip:dst_port`` for ``duration`` s.

    ``packets`` defaults to :data:`CRAFTED_NACKS`. A test that drives the
    accept path passes :func:`well_formed_nacks` output instead. The sender
    runs on ``host`` in the background and stops by itself. Datagrams sent
    before the session binds its RTCP port are lost, which is harmless.
    Returns the process handle. The caller waits on it.
    """
    if packets is None:
        packets = CRAFTED_NACKS
    script = _SENDER.format(
        hexes=[d.hex() for d in packets],
        duration=int(duration),
        ip=dst_ip,
        port=int(dst_port),
        interval=float(interval),
    )
    encoded = base64.b64encode(script.encode()).decode()
    command = f"python3 -c \"import base64; exec(base64.b64decode('{encoded}'))\""
    return run(command, host=host, timeout=duration + 30, background=True)
