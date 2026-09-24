# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""GStreamer framework adapter (unified Application model).

The :class:`mtl_engine.ffmpeg.FFmpeg` peer for the MTL GStreamer plugin
(``ecosystem/gstreamer_plugin``). One RX and one TX ``gst-launch-1.0`` pipeline
for st20p, st30p or st40p: ``create_command()`` builds the RX, ``execute_test()``
the TX, and runs them on one host the way FFmpeg does -- RX first, TX
``sleep_interval`` later, both stopped after ``test_time`` of wall clock -- then
compliance, integrity and ``validate_results()`` give the verdict. In a cross-app
test, RxTxApp runs in the place of the pipeline on its end.
"""

from __future__ import annotations

import logging
import math
import os
import re

from common.integrity.video_integrity import calculate_yuv_frame_size
from mtl_engine import ip_pools
from mtl_engine.application_base import Application, ProcSpec
from mtl_engine.config.mappings import APP_NAME_MAP
from mtl_engine.const import GSTREAMER_LIB_PATH, RXTXAPP_PATH
from mtl_engine.integrity import (
    get_channel_number,
    get_frame_sample_number,
    get_sample_size,
    min_expected_frames,
)
from mtl_engine.integrity_session import NO_INTEGRITY
from mtl_engine.media_files import pformat_to_exact_fps
from mtl_engine.pcap_compliance import NO_COMPLIANCE
from mtl_engine.rxtxapp import RxTxApp, check_rx_output

logger = logging.getLogger(__name__)

# rawvideoparse format per MTL pixel_format: the two formats the mtl_st20p_tx
# sink caps accept. The wire format is always YUV_422_10bit with BPM packing --
# gst_mtl_st20p_{tx,rx}.c set neither from a property.
_VIDEO_FORMATS = {"YUV422PLANAR10LE": "i422-10le", "v210": "v210"}
_PCM_FORMATS = {"PCM8": "s8", "PCM16": "s16be", "PCM24": "s24be"}
_PTIMES = {"1": "1ms", "0.12": "125us", "0.25": "250us", "0.33": "333us", "4": "4ms"}
_SAMPLING_HZ = {"48kHz": 48000, "96kHz": 96000}

# The ANC IDs RxTxApp's st40p sender uses (tx_st40p_app.c), so that every app
# puts the same packets on the wire; the plugin defaults to DID 0.
_ST40P_DID = 0x43
_ST40P_SDID = 0x02

# The cross-app pairs (tx_application, rx_application), by the end RxTxApp takes.
_RXTXAPP_END = {("rxtxapp", "gstreamer"): "tx", ("gstreamer", "rxtxapp"): "rx"}

# MTL's periodic RX stats, e.g. "RX_VIDEO_SESSION(0,0:st20src): fps 25.000227
# frames 250 pkts 1028753", over the interval since the last line.
_RX_STATS = re.compile(
    r"RX_(?:VIDEO|AUDIO|ANC)_SESSION\([^)]*\): fps ([\d.]+) frames (\d+)"
)


def _exact_fps(framerate) -> float:
    num, _, den = pformat_to_exact_fps(framerate).partition("/")
    return int(num) / int(den or 1)


class GStreamer(Application):
    """GStreamer framework adapter (single-host RX+TX orchestrator)."""

    def __init__(self, app_path=None, config_file_path=None):
        super().__init__(app_path, config_file_path)
        self._tx_stages: list[str] = []
        self._return_codes: list[tuple[str, int | None]] = []
        self._rxtxapp: RxTxApp | None = None
        self._rxtxapp_end: str | None = None

    def get_app_name(self) -> str:
        return "GStreamer"

    def get_executable_name(self) -> str:
        return APP_NAME_MAP["gstreamer"]

    def rx_recording_cap(self) -> int:
        """``rx_max_file_size`` becomes ``num-buffers`` on the st20p receiver."""
        if self.params.get("session_type") != "st20p":
            return 0
        return int(self.params.get("rx_max_file_size") or 0)

    # ----------------------------------------------------- command build
    def _create_command_and_config(self) -> tuple:
        """Return ``(rx_cmd, None)``; ``execute_test`` builds the TX pipeline and
        swaps in the RxTxApp end, if any."""
        session_type = self.params["session_type"]
        builders = {
            "st20p": self._build_st20p,
            "st30p": self._build_st30p,
            "st40p": self._build_st40p,
        }
        if session_type not in builders:
            raise ValueError(f"GStreamer adapter does not support {session_type}")
        nic_port_list = self.params["nic_port_list"]
        if not nic_port_list or len(nic_port_list) < 2:
            raise ValueError("nic_port_list needs a TX and an RX port")
        pair = (self.params["tx_application"], self.params["rx_application"])
        self._rxtxapp = None
        self._rxtxapp_end = _RXTXAPP_END.get(pair)
        if pair != (None, None) and not self._rxtxapp_end:
            raise ValueError(
                f"GStreamer adapter does not support {pair[0]} to {pair[1]}"
            )
        if self._rxtxapp_end:
            # RxTxApp stands in for the element on its end: the same NIC port
            # and stream, from the same test parameters.
            params = {k: self.params[k] for k in self._user_provided_params}
            params["direction"] = self._rxtxapp_end
            params["nic_port_list"] = [
                nic_port_list[0 if self._rxtxapp_end == "tx" else 1]
            ]
            self._rxtxapp = RxTxApp(RXTXAPP_PATH)
            self._rxtxapp.create_command(**params)
        if not self.params.get("output_file"):
            # st40p and cross-app tests assert on the wire and pass no
            # output_file, but the RX recording is what proves the frames arrived.
            self.params["output_file"] = os.path.join(
                os.path.dirname(self.params["input_file"]),
                f"gst_{session_type}_rx.out",
            )

        # Each builder returns the stages between the looping source and the TX
        # element plus the session properties of the TX and RX elements.
        tx_stages, tx_props, rx_props = builders[session_type]()
        self._tx_stages = [
            *tx_stages,
            f"mtl_{session_type}_tx {self._net_props(True)} {tx_props}",
        ]
        rx = [
            f"mtl_{session_type}_rx {self._net_props(False)} {rx_props}",
            f"filesink location={self.params['output_file']}",
        ]
        return self._pipeline(rx), None

    def _pipeline(self, stages: list[str]) -> str:
        # The path is relative to ``build``, the process cwd, like FFMPEG_EXE.
        return (
            f"{self.get_executable_path()} --gst-plugin-path={GSTREAMER_LIB_PATH} "
            + " ! ".join(stages)
        )

    def _dst_ip(self) -> str:
        if self.params["test_mode"] == "multicast":
            return ip_pools.rx_multicast[0]
        return ip_pools.rx[0]

    def _net_props(self, is_tx: bool) -> str:
        """Port and addresses, per RxTxApp's convention (rxtxapp.py).

        TX egresses ``nic_port_list[0]``, RX ingresses ``nic_port_list[1]``: the
        ``pcap_capture`` fixture sniffs the second NIC's PF. RX ``ip`` is the
        group to join, or the sender's address for unicast.
        """
        nic_port_list = self.params["nic_port_list"]
        multicast = self.params["test_mode"] == "multicast"
        return (
            f"dev-port={nic_port_list[0] if is_tx else nic_port_list[1]} "
            f"dev-ip={ip_pools.tx[0] if is_tx else ip_pools.rx[0]} "
            f"ip={self._dst_ip() if is_tx or multicast else ip_pools.tx[0]} "
            f"udp-port={self.params['port']} "
            f"payload-type={self.params['payload_type']}"
        )

    def _looping_source(self, host, test_time: int) -> str:
        """The TX input on a loop -- FFmpeg's ``-stream_loop -1``."""
        p = self.params
        if p["session_type"] != "st20p":
            return f"multifilesrc location={p['input_file']} loop=true"
        # multifilesrc reads the whole file into one buffer per loop -- 6 GB at
        # 4K, held twice across a loop -- and starves the TX while it reads.
        # Video instead reads one frame per buffer from enough copies of the
        # file to outlast the run. Each copy has its own parser: one behind
        # concat restarts PTS at 0 per copy inside segments that do not, which
        # puts every copy after the first outside its segment.
        frame_size = calculate_yuv_frame_size(
            p["width"], p["height"], p["pixel_format"]
        )
        frames = self._file_size(host, p["input_file"]) // frame_size
        if not frames:
            raise ValueError(f"{p['input_file']} holds no {frame_size} B frame")
        copies = math.ceil(test_time * _exact_fps(p["framerate"]) / frames)
        copy = (
            f"filesrc location={p['input_file']} blocksize={frame_size} ! "
            f"rawvideoparse format={_VIDEO_FORMATS[p['pixel_format']]} "
            f"width={p['width']} height={p['height']} "
            f"framerate={pformat_to_exact_fps(p['framerate'])} ! loop."
        )
        return f"{' '.join([copy] * copies)} concat name=loop"

    def _build_st20p(self) -> tuple[list[str], str, str]:
        p = self.params
        if p["transport_format"] != "YUV_422_10bit" or p["packing"] != "BPM":
            raise ValueError(
                "mtl_st20p_tx/rx carry YUV_422_10bit with BPM packing only, got "
                f"{p['transport_format']} {p['packing']}"
            )
        width, height, fps = (
            p["width"],
            p["height"],
            pformat_to_exact_fps(p["framerate"]),
        )
        rx_extra = ""
        cap = self.rx_recording_cap()
        if cap:
            frame_size = calculate_yuv_frame_size(width, height, p["pixel_format"])
            rx_extra = f" num-buffers={cap // frame_size}"
        # retry counts 1 s blocking gets; the default 10 gives up before TX
        # finishes pacing training -- FFmpeg's -init_retry 20 for the same reason.
        rx = (
            f"retry=20 rx-width={width} rx-height={height} rx-fps={fps} "
            f"rx-pixel-format={p['pixel_format']}{rx_extra}"
        )
        return [], "", rx

    def _build_st30p(self) -> tuple[list[str], str, str]:
        p = self.params
        ptime = _PTIMES[str(p["audio_ptime"])]
        sampling = _SAMPLING_HZ[p["audio_sampling"]]
        channels = get_channel_number(p["audio_channels"][0])
        tx = [
            f"rawaudioparse pcm-format={_PCM_FORMATS[p['audio_format']]} "
            f"sample-rate={sampling} num-channels={channels}",
        ]
        rx = (
            f"rx-audio-format={p['audio_format']} rx-channel={channels} "
            f"rx-sampling={sampling} rx-ptime={ptime}"
        )
        return tx, f"tx-ptime={ptime}", rx

    def _build_st40p(self) -> tuple[list[str], str, str]:
        fps = pformat_to_exact_fps(self.params["framerate"])
        tx_props = f"tx-fps={fps} tx-did={_ST40P_DID} tx-sdid={_ST40P_SDID}"
        return [], tx_props, ""

    # ----------------------------------------------------- execute_test
    def execute_test(  # type: ignore[override]
        self,
        build: str,
        test_time: int = 30,
        host=None,
        sleep_interval: int = 5,
        compliance=NO_COMPLIANCE,
        integrity=NO_INTEGRITY,
        fail_on_error: bool = True,
        **extra,
    ) -> bool:
        """Single-host RX-then-TX orchestrator, as :meth:`FFmpeg.execute_test`."""
        unsupported = sorted(
            k
            for k in ("tx_host", "rx_host", "rx_app", "tx_first")
            if extra.get(k) is not None
        )
        if unsupported:
            raise ValueError(
                f"GStreamer adapter does not support {unsupported}; "
                f"use a single ``host=`` argument."
            )
        if not host:
            raise ValueError("host required for single-host execution")
        if not self.command:
            raise RuntimeError("create_command() must be called before execute_test()")

        self.params["test_time"] = test_time
        self._output_files = [self.params["output_file"]]
        if self._rxtxapp:
            self._rxtxapp.prepare_execution(build=build, host=host)
        rx_cmd = self._rxtxapp.command if self._rxtxapp_end == "rx" else self.command
        if self._rxtxapp_end == "tx":
            tx_cmd = self._rxtxapp.command
        else:
            # Built here: looping the video takes the run length and the
            # input's frame count, which only the host can tell.
            tx_cmd = self._pipeline(
                [self._looping_source(host, test_time), *self._tx_stages]
            )
        specs = [
            ProcSpec(cmd=rx_cmd, host=host, label="RX", bounded=False),
            ProcSpec(cmd=tx_cmd, host=host, label="TX1", bounded=False),
        ]
        # Traffic flows only once TX is up, so the capture arms after the last start.
        intent = self.capture_intent()
        self._run_proc_group(
            specs,
            build=build,
            test_time=test_time,
            sleep_interval=sleep_interval,
            wall_clock_seconds=test_time,
            cleanup_host=host,
            after_last_start=lambda _proc: compliance.arm(intent),
        )
        self._return_codes = [(s.label, self._safe_return_code(s.proc)) for s in specs]
        self.last_output = specs[0].captured_output
        self.last_return_code = self._return_codes[0][1]

        return self._finalize_run(
            compliance,
            intent,
            fail_on_error,
            integrity=integrity,
            integrity_intents=self.integrity_intents(build, host),
        )

    def _resolve_capture_dst_ips(self) -> tuple[str, ...]:
        return (self._dst_ip(),)

    # ----------------------------------------------------- validate
    def _rx_frame_spec(self, host) -> tuple[int, float]:
        """``(frame size, frame rate)`` of the RX recording."""
        p = self.params
        if p["session_type"] == "st20p":
            frame_size = calculate_yuv_frame_size(
                p["width"], p["height"], p["pixel_format"]
            )
            return frame_size, _exact_fps(p["framerate"])
        if p["session_type"] == "st30p":
            ptime = str(p["audio_ptime"])
            frame_samples = get_frame_sample_number(p["audio_sampling"], ptime)
            frame_size = (
                get_sample_size(p["audio_format"])
                * frame_samples
                * get_channel_number(p["audio_channels"][0])
            )
            return frame_size, _SAMPLING_HZ[p["audio_sampling"]] / frame_samples
        # st40p: every frame carries the whole input file as its UDW.
        return self._file_size(host, p["input_file"]), _exact_fps(p["framerate"])

    def _rx_stats(self) -> tuple[list[float], int]:
        """MTL's RX stats: the rate of each full interval, and the frames in all.

        Traffic starts inside the first interval that carries frames, and RX is
        stopped while TX still sends, so its last line -- printed as it detaches
        -- ends a partial interval too. Both are left out; an empty interval
        between them is a stall and stays in. The list is empty for an st20p
        recording that ``num-buffers`` cuts short.
        """
        stats = _RX_STATS.findall(self.last_output or "")
        rates = [float(fps) for fps, _ in stats]
        busy = [i for i, fps in enumerate(rates) if fps]
        full = rates[busy[0] + 1 : -1] if busy else []
        return full, sum(int(frames) for _, frames in stats)

    @staticmethod
    def _file_size(host, path: str) -> int:
        res = host.connection.execute_command(
            f"stat -c %s {path}", expected_return_codes=None
        )
        return int(res.stdout.strip()) if res.return_code == 0 else 0

    def _validate_rxtxapp_rx(self, fail_on_error: bool) -> bool:
        """RxTxApp received: one OK result per session means each session's
        rate held within its 5 % (ST_APP_EXPECT_NEAR)."""
        errors = [f"{label} exited {rc}" for label, rc in self._return_codes if rc != 0]
        lines = (self.last_output or "").splitlines()
        if not check_rx_output(
            self._rxtxapp.config, lines, self.params["session_type"], False
        ):
            errors.append("RxTxApp RX reported no OK result")
        if errors:
            self._fail_validation(
                f"GStreamer test failed: {'; '.join(errors)}", fail_on_error
            )
            return False
        return True

    def validate_results(self, fail_on_error: bool = True) -> bool:  # type: ignore[override]
        if self._rxtxapp_end == "rx":
            return self._validate_rxtxapp_rx(fail_on_error)
        host = self._host
        out_file = self.params["output_file"]
        try:
            # Every MTL RX element ends with EOS -- exit code 0 -- when its
            # retries or timeout run out, so a receiver that never got a frame
            # still exits 0: the recording and the rate carry the verdict.
            errors = [
                f"{label} exited {rc}" for label, rc in self._return_codes if rc != 0
            ]
            frame_size, fps = self._rx_frame_spec(host)
            rates, received = self._rx_stats()
            min_frames = min_expected_frames(fps, self.params["test_time"])
            if self.rx_recording_cap():
                cap = self.rx_recording_cap() // frame_size
                min_frames = min(min_frames, max(1, cap))
                received = min(received, cap)
            # MTL's session counts a frame before the pipeline takes it, so this
            # is the rate off the wire: within RxTxApp's 5 % of nominal
            # (ST_APP_EXPECT_NEAR in rx_*_app.c) over each full interval, so
            # that a stall cannot average out.
            worst = max(rates, key=lambda r: abs(r - fps), default=None)
            if worst is None:
                logger.info("RX rate not checked: no full RX stats interval")
            elif abs(worst - fps) > fps * 0.05:
                errors.append(f"RX ran at {worst:.3f} fps, expected {fps:.3f}")
            size = self._file_size(host, out_file)
            recorded = size // frame_size
            if recorded < min_frames:
                errors.append(
                    f"{out_file} holds {recorded} frames of "
                    f"{frame_size} B, expected >= {min_frames}"
                )
            elif recorded < received * 0.95:
                # The pipeline drops what arrives while the app holds every
                # framebuffer (st*_pipeline_rx.c). A few frames are in flight
                # when RX stops, well inside RxTxApp's 5 %.
                errors.append(
                    f"{out_file} holds {recorded} of the {received} frames "
                    f"MTL received"
                )
            elif self.params["session_type"] == "st40p":
                # No integrity check covers ANC. The recording repeats the UDW
                # iff its first frame is the input and it equals itself shifted
                # by one frame.
                res = host.connection.execute_command(
                    f"cmp -s -n {frame_size} {self.params['input_file']} {out_file} && "
                    f"cmp -s -n {size - frame_size} {out_file} {out_file} {frame_size} 0",
                    shell=True,
                    expected_return_codes=None,
                )
                if res.return_code != 0 or size % frame_size:
                    errors.append(f"{out_file} is not the input UDW repeated")
            self._cleanup_output_files(host)
        except AssertionError:
            raise
        except Exception as e:
            self._fail_validation(f"GStreamer validation error: {e}", fail_on_error)
            return False

        if errors:
            self._fail_validation(
                f"GStreamer test failed: {'; '.join(errors)}", fail_on_error
            )
            return False
        return True
