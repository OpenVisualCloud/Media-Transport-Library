# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

"""The capture filter, and which captures may be truncated to the -20 header.

Two things have to hold: the generated BPF program must select exactly the
packets the ``dst <ip>`` expression it replaces did and truncate them to the
asked-for length, and truncation may only be asked for on a capture holding
nothing but ST 2110-20 video (see ``NetsniffRecorder.update_filter``).

The program is checked by executing it, not by restating how it is encoded: a
wrong jump offset accepts or drops the wrong packet, which no comparison
against the generator's own arithmetic would notice.

The acceptance tree imports packages only its venv installs. Stand them in,
since none is reached by the code under test, and import the real modules: a
stub of the code itself would not notice a rename or a changed signature.
"""

import ipaddress
import logging
import re
import sys
import types
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

sys.path.append(str(Path(__file__).resolve().parents[1] / "acceptance"))
for _name in (
    "mfd_common_libs",
    "mfd_common_libs.log_levels",
    "mfd_connect",
    "mfd_connect.exceptions",
    "pytest",
    "pytest_check",
    "requests",
):
    sys.modules.setdefault(_name, types.ModuleType(_name))
sys.modules["mfd_common_libs.log_levels"].TEST_FAIL = 41
sys.modules["pytest_check"].check = Mock()
sys.modules["mfd_connect"].SSHConnection = Mock
# Real exception classes: netsniff and pcap_compliance catch these around the
# capture lifecycle, and a Mock cannot appear in an except clause.
for _exception in (
    "ConnectionCalledProcessError",
    "RemoteProcessInvalidState",
    "RemoteProcessTimeoutExpired",
    "SSHRemoteProcessEndException",
):
    setattr(sys.modules["mfd_connect.exceptions"], _exception, OSError)

from create_pcap_file import netsniff  # noqa: E402
from create_pcap_file.netsniff import (  # noqa: E402
    _SNAPLEN_FULL,
    _ST2110_20_SNAPLEN,
    NetsniffRecorder,
    _bpf_program,
)
from mtl_engine import ip_pools, pcap_compliance  # noqa: E402
from mtl_engine.application_base import Application  # noqa: E402
from mtl_engine.config.universal_params import UNIVERSAL_PARAMS  # noqa: E402
from mtl_engine.pcap_compliance import CaptureIntent, ComplianceSession  # noqa: E402
from mtl_engine.rxtxapp import RxTxApp  # noqa: E402

for _module in (netsniff, pcap_compliance):
    _module.logger.addHandler(logging.NullHandler())

_INSTRUCTION = re.compile(r"^\{ (0x[0-9a-f]+), (\d+), (\d+), (0x[0-9a-f]+) \},$")
_LOAD_WIDTH = {0x28: 2, 0x30: 1, 0x20: 4}  # ldh, ldb, ld


def _run_bpf(program: str, packet: bytes) -> int:
    """Execute a netsniff-ng filter file against *packet*, returning its verdict.

    Covers only the five opcodes :func:`_bpf_program` emits; anything else is a
    generator change this test has not been taught about. An out-of-range load
    drops the packet, as the kernel's interpreter does.
    """
    instructions = []
    for line in program.splitlines():
        match = _INSTRUCTION.match(line)
        assert match, f"unparsable filter line: {line!r}"
        instructions.append(tuple(int(field, 0) for field in match.groups()))

    accumulator = 0
    counter = 0
    for _ in range(len(instructions)):
        code, jump_true, jump_false, value = instructions[counter]
        counter += 1
        if code in _LOAD_WIDTH:
            width = _LOAD_WIDTH[code]
            if value + width > len(packet):
                return 0
            accumulator = int.from_bytes(packet[value : value + width], "big")
        elif code == 0x15:  # jeq #value
            counter += jump_true if accumulator == value else jump_false
        elif code == 0x06:  # ret #value
            return value
        else:
            raise AssertionError(f"unhandled BPF opcode {code:#x}")
    raise AssertionError("filter ran off the end without returning")


def _packet(dst_ip: str, protocol: int = 17, ethertype: int = 0x0800) -> bytes:
    """An Ethernet/IPv4/UDP frame, laid out at the offsets the filter loads."""
    return (
        b"\x01\x00\x5e\x00\x00\x01"  # 0  Ethernet destination
        + b"\x02" * 6  # 6  Ethernet source
        + ethertype.to_bytes(2, "big")  # 12 ethertype
        + b"\x45\x00"  # 14 version/IHL, DSCP
        + (1000).to_bytes(2, "big")  # 16 total length
        + bytes(5)  # 18 identification, flags, TTL
        + bytes([protocol])  # 23 protocol
        + bytes(2)  # 24 header checksum
        + bytes([192, 168, 0, 1])  # 26 source address
        + ipaddress.IPv4Address(dst_ip).packed  # 30 destination address
        + bytes(966)
    )


class BpfFilterProgramTests(unittest.TestCase):
    """What the generated program selects, and how much of it it keeps."""

    def test_single_address_matches_bpfc_output(self):
        """``bpfc -f netsniff-ng`` is the oracle, byte for byte.

        Compiled from the equivalent assembly (``ldh [12]`` / ``jneq #0x800,
        drop`` / ``ldb [23]`` / ``jneq #17, drop`` / ``ld [30]`` / ``jneq
        #0xef630401, drop`` / ``ret #128`` / ``drop: ret #0``) by an
        independent compiler, so this pins the record format netsniff-ng's
        ``bpf_parse_rules()`` accepts as well as the encoding.
        """
        self.assertEqual(
            _bpf_program(("239.99.4.1",), _ST2110_20_SNAPLEN),
            "{ 0x28, 0, 0, 0x0000000c },\n"
            "{ 0x15, 0, 5, 0x00000800 },\n"
            "{ 0x30, 0, 0, 0x00000017 },\n"
            "{ 0x15, 0, 3, 0x00000011 },\n"
            "{ 0x20, 0, 0, 0x0000001e },\n"
            "{ 0x15, 0, 1, 0xef630401 },\n"
            "{ 0x6, 0, 0, 0x00000080 },\n"
            "{ 0x6, 0, 0, 0x00000000 },\n",
        )

    def test_every_listed_address_is_accepted_at_the_snaplen(self):
        """Each address must reach the accept, not just the last one.

        An ST 2022-7 session puts both of its copies in one pcap, so the
        forward jump over the remaining comparisons has to be right for every
        address in the list.
        """
        addresses = ("239.0.0.1", "239.0.0.2", "239.0.0.3")
        for count in range(1, len(addresses) + 1):
            listed = addresses[:count]
            program = _bpf_program(listed, _ST2110_20_SNAPLEN)
            for address in listed:
                with self.subTest(count=count, address=address):
                    self.assertEqual(
                        _run_bpf(program, _packet(address)), _ST2110_20_SNAPLEN
                    )

    def test_unlisted_address_is_dropped(self):
        addresses = ("239.0.0.1", "239.0.0.2", "239.0.0.3")
        for count in range(1, len(addresses) + 1):
            with self.subTest(count=count):
                program = _bpf_program(addresses[:count], _ST2110_20_SNAPLEN)
                self.assertEqual(_run_bpf(program, _packet("239.0.0.9")), 0)

    def test_only_ipv4_udp_is_accepted(self):
        """The two early drops have to skip the whole address list.

        Their offsets are the only ones that depend on how many addresses
        follow, so a miscount lands inside the comparisons and accepts TCP or
        ARP addressed to the stream.
        """
        addresses = ("239.0.0.1", "239.0.0.2", "239.0.0.3")
        for count in range(1, len(addresses) + 1):
            program = _bpf_program(addresses[:count], _ST2110_20_SNAPLEN)
            with self.subTest(count=count):
                self.assertEqual(_run_bpf(program, _packet("239.0.0.1", protocol=6)), 0)
                self.assertEqual(
                    _run_bpf(program, _packet("239.0.0.1", ethertype=0x0806)), 0
                )

    def test_full_snaplen_keeps_the_whole_packet(self):
        program = _bpf_program(("239.0.0.1",), _SNAPLEN_FULL)
        self.assertEqual(_run_bpf(program, _packet("239.0.0.1")), 65535)

    def test_malformed_address_is_refused(self):
        with self.assertRaises(ValueError):
            _bpf_program(("239.0.0",), _ST2110_20_SNAPLEN)

    def test_empty_address_list_is_refused(self):
        """With nothing to compare, the accept would match every IPv4/UDP packet."""
        with self.assertRaises(ValueError):
            _bpf_program((), _ST2110_20_SNAPLEN)


class CaptureCommandTests(unittest.TestCase):
    """How the program reaches netsniff-ng."""

    def _recorder(self):
        connection = Mock()
        connection.start_process.return_value = Mock(running=True, pid=4242)
        connection.execute_command.return_value = Mock(
            return_code=0, stdout="capture-host\n", stderr=""
        )
        host = SimpleNamespace(name="capture-host", connection=connection)
        return NetsniffRecorder(
            host=host, test_name="case", pcap_dir="/mnt/ramdisk/pcap", interface="eth1"
        )

    def _started_command(self, recorder):
        self.assertTrue(recorder.start())
        return recorder.host.connection.start_process.call_args[0][0]

    def test_program_is_piped_into_stdin(self):
        recorder = self._recorder()
        recorder.update_filter(dst_ip="239.0.0.1", st2110_20_only=True)
        command = self._started_command(recorder)

        self.assertIn("| netsniff-ng", command)
        self.assertIn("-f -", command)
        # The command is one shell line over SSH, so the record separators have
        # to travel as printf escapes; a raw newline would truncate the program
        # to its first instruction and netsniff-ng would refuse to start.
        self.assertNotIn("\n", command)
        escaped = re.search(r"printf '%b' '([^']*)'", command)
        self.assertIsNotNone(escaped, command)
        self.assertEqual(
            escaped.group(1).replace("\\n", "\n"), recorder._filter_program
        )
        # Single-quoted in the shell command, so the program must not contain a
        # quote of its own.
        self.assertNotIn("'", recorder._filter_program)

    def test_pipeline_stays_inside_a_single_child(self):
        """No pipe may appear at the top level of the SSH command.

        mfd_connect resolves the PID by listing the wrapper shell's children and
        mis-handles more than one: a top-level pipeline briefly shows two, which
        either raises SSHPIDException out of start_process or leaves .running
        False, and start() then clears pcap_file without reaping -- leaving a
        root netsniff-ng holding the interface for every later test.
        """
        for st2110_20_only in (False, True):
            recorder = self._recorder()
            recorder.update_filter(dst_ip="239.0.0.1", st2110_20_only=st2110_20_only)
            command = self._started_command(recorder)
            with self.subTest(st2110_20_only=st2110_20_only):
                self.assertTrue(command.startswith('sudo sh -c "'), command)
                self.assertTrue(command.endswith('"'), command)
                # Everything after the opening quote is one argument to sh.
                self.assertNotIn('"', command[len('sudo sh -c "') : -1])
                self.assertLess(command.index('sh -c "'), command.index("|"))

    def test_unfiltered_capture_asks_for_no_filter(self):
        command = self._started_command(self._recorder())
        self.assertNotIn("-f -", command)
        self.assertNotIn("printf", command)
        self.assertNotIn("|", command)

    def test_filter_defaults_to_the_whole_packet(self):
        recorder = self._recorder()
        recorder.update_filter(dst_ip="239.0.0.1")
        self.assertEqual(
            _run_bpf(recorder._filter_program, _packet("239.0.0.1")), _SNAPLEN_FULL
        )

    def test_video_only_filter_truncates_to_the_headers(self):
        recorder = self._recorder()
        recorder.update_filter(dst_ip="239.0.0.1", st2110_20_only=True)
        self.assertEqual(
            _run_bpf(recorder._filter_program, _packet("239.0.0.1")),
            _ST2110_20_SNAPLEN,
        )

    def test_redundant_addresses_are_both_captured(self):
        recorder = self._recorder()
        recorder.update_filter(dst_ip=["239.0.0.1", "239.0.0.2"])
        for address in ("239.0.0.1", "239.0.0.2"):
            with self.subTest(address=address):
                self.assertEqual(
                    _run_bpf(recorder._filter_program, _packet(address)), _SNAPLEN_FULL
                )


class SnaplenScopeTests(unittest.TestCase):
    """Which captures may be truncated."""

    def _armed_truncation(self, **intent_fields):
        recorder = Mock(packets_capture=None)
        session = ComplianceSession(
            recorder=recorder,
            ebu_server={"ip": "10.0.0.1"},
            mtl_path="/mtl",
            node_id="case",
        )
        session.arm(
            CaptureIntent(
                dst_ips=("239.0.0.1",),
                capture_time=1,
                settle_time=0,
                **intent_fields,
            )
        )
        return recorder.update_filter.call_args.kwargs["st2110_20_only"]

    def test_video_only_capture_is_truncated(self):
        self.assertTrue(self._armed_truncation(st2110_20_only=True))

    def test_mixed_media_capture_is_captured_whole(self):
        self.assertFalse(self._armed_truncation(st2110_20_only=False))

    def test_capture_is_whole_unless_asked_otherwise(self):
        """The default has to be the safe one, for an intent built elsewhere."""
        self.assertFalse(self._armed_truncation())


class BaseApplicationScopeTests(unittest.TestCase):
    """The base Application must not truncate on the strength of its params."""

    def test_default_params_do_not_truncate(self):
        """``session_type`` defaults to ``st20p``, so it cannot be the deciding key.

        FFmpeg resolves its media type from its own ``mode`` key and falls back
        to this default, so a predicate reading ``session_type`` would truncate
        an FFmpeg audio capture that never mentioned video -- and EBU LIST would
        report the unanalysed audio stream as compliant.
        """
        self.assertEqual(UNIVERSAL_PARAMS["session_type"], "st20p")
        self.assertFalse(
            Application._st2110_20_only(SimpleNamespace(params=UNIVERSAL_PARAMS))
        )


class RxTxAppSnaplenScopeTests(unittest.TestCase):
    """RxTxApp decides from the config it generated, not from its params.

    ``create_command`` pops ``sessions=``, so ``params["session_type"]`` keeps
    only the *first* session's type -- ``st20p`` for the mixed-media
    ``native_af_xdp`` case. A predicate reading params therefore calls that
    capture video-only, truncates a pcap carrying ancillary data, and EBU LIST
    reports the unanalysed ancillary stream as compliant. So these drive the
    real ``create_command`` and read the answer off ``capture_intent()``: a
    predicate wired to the wrong source, or an intent that stops carrying the
    answer, both fail here.
    """

    @classmethod
    def setUpClass(cls):
        if not ip_pools.tx:  # module-level pools, appended to by init()
            ip_pools.init(1)

    @staticmethod
    def _app(**kwargs):
        app = RxTxApp(app_path="/mtl")
        app.create_command(
            nic_port_list=["native_af_xdp:eth2", "native_af_xdp:eth3"],
            test_mode="multicast",
            **kwargs,
        )
        return app

    _VIDEO = {
        "session_type": "st20p",
        "width": 1920,
        "height": 1080,
        "framerate": "p59",
        "input_file": "/mnt/media/video.yuv",
        "output_file": "/mnt/media/video.yuv",
    }
    _AUDIO = {
        "session_type": "st30p",
        "audio_format": "PCM24",
        "audio_channels": ["U02"],
        "audio_sampling": "48kHz",
        "audio_ptime": "1",
        "input_file": "/mnt/media/audio.pcm",
        "output_file": "/mnt/media/audio.pcm",
    }
    _ANCILLARY = {
        "session_type": "ancillary",
        "type_mode": "frame",
        "ancillary_format": "closed_caption",
        "ancillary_fps": "p50",
        "ancillary_url": "/mnt/media/anc.txt",
    }

    def test_mixed_media_capture_is_not_truncated(self):
        app = self._app(sessions=[self._VIDEO, self._AUDIO, self._ANCILLARY])
        # The trap the params-based predicate fell into.
        self.assertEqual(app.params.get("session_type"), "st20p")
        self.assertEqual(
            app._get_all_session_types_from_config(app.config),
            ["st20p", "st30p", "ancillary"],
        )
        self.assertFalse(app.capture_intent().st2110_20_only)

    def test_video_only_capture_is_truncated(self):
        app = self._app(**self._VIDEO)
        self.assertTrue(app.capture_intent().st2110_20_only)

    def test_several_video_sessions_are_truncated(self):
        app = self._app(sessions=[self._VIDEO, dict(self._VIDEO)])
        self.assertTrue(app.capture_intent().st2110_20_only)

    def test_audio_only_capture_is_not_truncated(self):
        app = self._app(**self._AUDIO)
        self.assertFalse(app.capture_intent().st2110_20_only)

    def test_capture_without_a_config_is_not_truncated(self):
        """No config means nothing is known about the media, so capture whole."""
        app = RxTxApp(app_path="/mtl")
        self.assertFalse(app._st2110_20_only())


if __name__ == "__main__":
    unittest.main()
