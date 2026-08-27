# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""GStreamer framework adapter (unified Application model).

Peer of :mod:`mtl_engine.rxtxapp` and :mod:`mtl_engine.ffmpeg`: the shared
protocol tests select it with ``@pytest.mark.parametrize("application",
[..., "gstreamer"])`` and drive it through the same
``create_command(**universal_params)`` / ``execute_test(...)`` pair, so a
GStreamer case costs one list entry instead of a parallel test tree.

Topology (single host, mirroring the FFmpeg adapter):
    1. ``create_command(...)`` builds the RX pipeline into ``self.command`` and
       the TX pipeline into ``self._tx_commands``.
    2. ``prepare_execution(build, host)`` resolves the RX output path and
       verifies the MTL plugin is loadable.
    3. ``execute_test(...)`` starts RX, waits ``sleep_interval``, starts TX,
       runs for ``test_time`` plus any PTP sync allowance, then stops both with
       the base class' SIGINT -> SIGKILL ladder (``gst-launch-1.0`` shuts the
       pipeline down cleanly on SIGINT and exits 0).
    4. ``validate_results()`` applies the oracles described below.

TX reads a *real* media asset and replays it for the whole window (see
:meth:`GStreamer._looping_source`), the GStreamer analogue of FFmpeg's
``-stream_loop -1`` -- never a synthetic pattern. ``rawvideoparse`` /
``rawaudioparse`` re-chunk the byte stream into correctly sized frames, so no
per-format ``blocksize`` arithmetic is needed, and the MTL sink's blocking frame
get paces the loop.

Oracles (``validate_results``), in order of strength:
    1. Every ``gst-launch-1.0`` exited 0. Measured behaviour: unknown element
       or property -> 1, runtime error -> 255, handled SIGINT -> 0 once the
       grace period covers MTL/DPDK teardown (see :data:`_STOP_GRACEFUL_S`).
       Only a code our own stop ladder produced (SIGKILL, or none at all) is
       tolerated, with a warning.
    2. No ``ERROR:`` / ``erroneous pipeline`` line in either pipeline's output.
    3. The RX output file exists and is non-empty; where the expected byte rate
       follows exactly from the session parameters (st20p, st30p) it must also
       hold at least :data:`_MIN_CAPTURE_RATIO` of a full-length capture, which
       is what rules out a pipeline that streamed briefly and then stopped.
    4. MTL's own pipeline stats must report frames put on both the TX and the RX
       side. Where oracle 3 cannot bound the run (st40p, whose payload size per
       frame is the sender's choice), missing stats fail and the per-interval
       frame counts after startup must additionally hold
       :data:`_MIN_STEADY_FRAME_RATIO` of the rate the session was configured
       for -- otherwise "one frame moved" would be st40p's whole throughput
       oracle.
Compliance (EBU) and integrity (MD5) are evaluated by the base class through
``capture_intent`` / ``integrity_intent`` exactly as for the other adapters --
this adapter adds no private validation path.
"""

from __future__ import annotations

import ipaddress
import logging
from typing import Optional

from mtl_engine import ip_pools
from mtl_engine.application_base import Application, ProcSpec
from mtl_engine.config.mappings import (
    APP_NAME_MAP,
    GSTREAMER_ST20P_FORMAT_MAP,
    MTL_DEFAULT_PACKING,
    audio_channel_count,
    audio_sampling_hz,
    gstreamer_audio_format,
    gstreamer_framerate,
    gstreamer_ptime,
    gstreamer_video_format,
)
from mtl_engine.const import GSTREAMER_LIB_PATH
from mtl_engine.integrity_session import NO_INTEGRITY
from mtl_engine.pcap_compliance import NO_COMPLIANCE

logger = logging.getLogger(__name__)


# Element introspection tool, used to turn "plugin not built" into an explicit
# environment error instead of an opaque pipeline-parse failure.
_GST_INSPECT = "gst-inspect-1.0"

# MTL element pair per session type: (TX sink, RX source).
_ELEMENTS = {
    "st20p": ("mtl_st20p_tx", "mtl_st20p_rx"),
    "st30p": ("mtl_st30p_tx", "mtl_st30p_rx"),
    "st40p": ("mtl_st40p_tx", "mtl_st40p_rx"),
}

# Plugin search path, relative to the build tree. ``.github/scripts/acceptance_setup.sh``
# produces both: the meson build directory and the install prefix.
_PLUGIN_DIRS = ("ecosystem/gstreamer_plugin/builddir", GSTREAMER_LIB_PATH)

# ``mtl_st20p_rx`` does a blocking frame get with a 1s timeout and gives up with
# GST_FLOW_EOS after ``retry`` attempts (gst_mtl_st20p_rx.c). The default of 10
# is shorter than TX-side DPDK EAL init plus pacing training, so RX would tear
# down before the first frame arrives. Same role as FFmpeg's ``-init_retry 20``;
# sized to outlast the whole default test window.
_ST20P_RX_RETRY = 45

# The only wire format the st20p elements can carry: ST20_FMT_YUV_422_10BIT is
# assigned unconditionally in gst_mtl_st20p_{tx,rx}.c.
_ST20P_TRANSPORT_FORMAT = "YUV_422_10bit"

# ST 2110-30 channel ceiling from the st30p pad templates
# (``channels = (int) [1, 8]`` in gst_mtl_st30p_{tx,rx}.c).
_ST30P_MAX_CHANNELS = 8

# Bytes per audio sample per channel, per MTL audio_format.
_AUDIO_SAMPLE_BYTES = {"PCM8": 1, "PCM16": 2, "PCM24": 3}

# ANC data/secondary-data ID. Matches what RxTxApp's st40p sender transmits
# (``meta[0].did = 0x43; meta[0].sdid = 0x02;`` in
# tests/tools/RxTxApp/src/tx_st40p_app.c) so both applications put the same
# ancillary packet on the wire and a compliance verdict is comparable.
_ST40P_DID = 0x43
_ST40P_SDID = 0x02

# Minimum fraction of a full-length capture the RX side must produce. This
# bounds how long the stream ran; it is not a pacing grade. RX starts first and
# both processes pay DPDK EAL init out of the same wall-clock budget, and TX
# re-reads the asset at every loop wrap, so a healthy run lands well under 100%
# and the floor has to sit below the worst honest case rather than near 1.0.
# It assumes the output mount can hold a full-length dump: if it cannot, this
# stops measuring MTL and starts measuring free space. Sizing that mount is
# configs/gen_config.py::_media_ramdisk_gib, which derives it from test_time.
_MIN_CAPTURE_RATIO = 0.5

# Minimum fraction of the nominal frame rate MTL must sustain once the stream is
# up. Far tighter than _MIN_CAPTURE_RATIO because it grades only whole stats
# intervals after startup, so none of the slack that ratio spends on process
# launch is needed here. Measured on st40p at test_time 30 (two printed
# intervals) and 90 (eight): every steady interval came in within one frame of
# nominal, p29 at 103.4% and p59 at 101.6-101.7% (extract_framerate truncates
# those to 29 and 59 against a real 29.97 and 59.94). p50 sets the bound at
# exactly 100.0%, having no truncation to round down, so a floor above 1.0 minus
# per-interval jitter would fail it -- raise this only with fresh p50 data. A
# stream delivering half the requested rate measures 50.8%, which 0.5 passed by
# four frames and this rejects with 10 points to spare.
_MIN_STEADY_FRAME_RATIO = 0.9

# MTL prints its pipeline stats once per MT_STAT_INTERVAL_S_DEFAULT (the library
# also exposes this as mtl_init_params.dump_period_s, which nothing in the
# acceptance path sets). One line per *completed* interval, counters reset in the
# print, and the pipeline layer unregisters its stat before teardown so there is
# no partial final dump -- hence a healthy run of ``test_time`` seconds prints
# ``test_time // _MTL_STATS_INTERVAL_S - 1`` of them. Measured: 30s -> 2, 90s ->
# 8. If anyone gives the pipeline layer a teardown dump the way the transport
# layer has one, the last element becomes a partial interval and the rate floor
# below starts false-failing.
_MTL_STATS_INTERVAL_S = 10

# Intervals needed before a rate can be graded: one to discard because it
# overlaps process startup, one to measure.
_MIN_GRADED_INTERVALS = 2

# Seconds to let a pipeline finish on SIGINT before the base class escalates.
# Measured on an st30p run (RX + TX, one MTL session each): RX exited 0.6s after
# ``kill -2``, TX took 21.7s, all of it inside ``Setting pipeline to NULL`` ->
# MTL session free -> DPDK cleanup, so the 10s universal default always SIGKILLed
# the TX pipeline. This covers the measured cost with margin; a larger geometry
# can still exceed it, which _check_pipeline_exit() treats as a warning.
_STOP_GRACEFUL_S = 30


class GStreamer(Application):
    """MTL GStreamer plugin adapter (single-host RX+TX orchestrator)."""

    def __init__(self, app_path=None, config_file_path=None):
        super().__init__(app_path, config_file_path)
        self._tx_commands: list[str] = []
        self._build: str | None = None
        self._plugin_path: str | None = None
        # One record per pipeline of the last run: label, return code, output.
        self._results: list[dict] = []

    # ------------------------------------------------------------------ ABCs
    def get_app_name(self) -> str:
        return "GStreamer"

    def get_executable_name(self) -> str:
        return APP_NAME_MAP["gstreamer"]

    # ----------------------------------------------------- capabilities
    def unsupported_reason(self, **params) -> Optional[str]:
        """Report plugin gaps so a shared test can skip without naming the app.

        Only genuine limitations of the MTL GStreamer plugin are listed; every
        parameter combination not mentioned here is expected to work.
        """
        if params.get("enable_rtcp"):
            # gst_mtl_common.c installs no RTCP property on any MTL element, so
            # there is no way to ask the plugin for an RTCP-enabled session.
            return "MTL GStreamer plugin exposes no RTCP property (gst_mtl_common.c)"
        session_type = params.get("session_type")
        if session_type == "st20p":
            pixel_format = params.get("pixel_format")
            if (
                pixel_format is not None
                and pixel_format not in GSTREAMER_ST20P_FORMAT_MAP
            ):
                return (
                    f"MTL GStreamer st20p plugin converts only "
                    f"{sorted(GSTREAMER_ST20P_FORMAT_MAP)} "
                    f"(gst_mtl_st20p_rx.c), not {pixel_format}"
                )
            transport_format = params.get("transport_format")
            if transport_format not in (None, _ST20P_TRANSPORT_FORMAT):
                return (
                    f"MTL GStreamer st20p plugin hardcodes "
                    f"{_ST20P_TRANSPORT_FORMAT} on the wire "
                    f"(gst_mtl_st20p_tx.c), not {transport_format}"
                )
            packing = params.get("packing")
            if packing not in (None, MTL_DEFAULT_PACKING):
                # No element installs a packing property, so the session keeps
                # the library default; accepting GPM here would report a pass
                # for a mode that never reached the wire.
                return (
                    f"MTL GStreamer st20p plugin has no packing property and "
                    f"leaves the library default {MTL_DEFAULT_PACKING}, "
                    f"not packing={packing}"
                )
        if session_type == "st30p":
            channels = params.get("audio_channels")
            if channels is not None:
                count = audio_channel_count(channels)
                if count > _ST30P_MAX_CHANNELS:
                    return (
                        f"MTL GStreamer st30p pad templates cap channels at "
                        f"{_ST30P_MAX_CHANNELS}; {channels} needs {count}"
                    )
        return None

    # ----------------------------------------------------- command build
    def _create_command_and_config(self) -> tuple:
        """Build the RX pipeline (returned) and the TX pipeline (``_tx_commands``).

        The RX command carries an ``{out}`` placeholder for the ``filesink``
        location; :meth:`prepare_execution` fills it in once the host is known.
        """
        session_type = self.params.get("session_type")
        if session_type not in _ELEMENTS:
            raise ValueError(
                f"GStreamer adapter supports {sorted(_ELEMENTS)}, "
                f"got session_type={session_type!r}"
            )
        nic_port_list = self.params["nic_port_list"]
        if not nic_port_list or len(nic_port_list) < 2:
            raise ValueError(
                "nic_port_list with a TX and an RX port is required "
                f"(got {nic_port_list!r})"
            )
        if not self.params.get("input_file"):
            raise ValueError("input_file is required")

        builder = {
            "st20p": self._build_st20p_cmds,
            "st30p": self._build_st30p_cmds,
            "st40p": self._build_st40p_cmds,
        }[session_type]
        tx_cmd, rx_cmd = builder(nic_port_list)
        self._tx_commands = [tx_cmd]
        return rx_cmd, None

    # -- session: st20p ---------------------------------------------------
    def _build_st20p_cmds(self, nic_port_list) -> tuple[str, str]:
        pixel_format = self.params["pixel_format"]
        transport_format = self.params["transport_format"]
        if transport_format != _ST20P_TRANSPORT_FORMAT:
            # Invariant restated: a test that skipped on unsupported_reason()
            # never gets here, and anything else is a caller bug worth raising.
            raise ValueError(
                f"MTL GStreamer st20p plugin transmits "
                f"{_ST20P_TRANSPORT_FORMAT} only, "
                f"got transport_format={transport_format!r}"
            )
        width = int(self.params["width"])
        height = int(self.params["height"])
        framerate = gstreamer_framerate(self.params["framerate"])

        tx_cmd = self._pipeline(
            self._looping_source(),
            # Caps are the only route to ops_tx.interlaced: gst_mtl_st20p_tx.c
            # installs no property and reads the negotiated interlace-mode.
            f"rawvideoparse format={gstreamer_video_format(pixel_format)} "
            f"width={width} height={height} framerate={framerate} "
            f"interlaced={_gst_bool(self.params.get('interlaced'))}",
            self._mtl_element("st20p", is_tx=True, nic_port_list=nic_port_list),
        )
        rx_cmd = self._pipeline(
            self._mtl_element(
                "st20p",
                is_tx=False,
                nic_port_list=nic_port_list,
                extra=[
                    f"retry={_ST20P_RX_RETRY}",
                    f"rx-width={width}",
                    f"rx-height={height}",
                    f"rx-fps={framerate}",
                    f"rx-pixel-format={pixel_format}",
                    f"rx-interlaced={_gst_bool(self.params.get('interlaced'))}",
                ],
            ),
            "filesink location={out}",
        )
        return tx_cmd, rx_cmd

    # -- session: st30p ---------------------------------------------------
    def _build_st30p_cmds(self, nic_port_list) -> tuple[str, str]:
        audio_format = self.params["audio_format"]
        channels = audio_channel_count(self.params["audio_channels"])
        sampling = audio_sampling_hz(self.params["audio_sampling"])
        ptime = gstreamer_ptime(self.params["audio_ptime"])
        gst_format = gstreamer_audio_format(audio_format)

        tx_cmd = self._pipeline(
            self._looping_source(),
            f"rawaudioparse pcm-format={gst_format} sample-rate={sampling} "
            f"num-channels={channels}",
            self._mtl_element(
                "st30p",
                is_tx=True,
                nic_port_list=nic_port_list,
                extra=[f"tx-ptime={ptime}"],
            ),
        )
        rx_cmd = self._pipeline(
            self._mtl_element(
                "st30p",
                is_tx=False,
                nic_port_list=nic_port_list,
                extra=[
                    f"rx-audio-format={audio_format}",
                    f"rx-channel={channels}",
                    f"rx-sampling={sampling}",
                    f"rx-ptime={ptime}",
                ],
            ),
            "filesink location={out}",
        )
        return tx_cmd, rx_cmd

    # -- session: st40p ---------------------------------------------------
    def _build_st40p_cmds(self, nic_port_list) -> tuple[str, str]:
        framerate = gstreamer_framerate(self.params["framerate"])
        tx_cmd = self._pipeline(
            self._looping_source(),
            self._mtl_element(
                "st40p",
                is_tx=True,
                nic_port_list=nic_port_list,
                extra=[
                    f"tx-fps={framerate}",
                    f"tx-did={_ST40P_DID}",
                    f"tx-sdid={_ST40P_SDID}",
                    "input-format=raw-udw",
                    f"tx-interlaced={_gst_bool(self.params.get('interlaced'))}",
                    # ST40P_TX_FLAG_SPLIT_ANC_BY_PKT (gst_mtl_st40p_tx.c:511):
                    # one ANC packet per RTP packet instead of several packed
                    # into one, which ST 2110-40 also permits.
                    "split-anc-by-pkt="
                    f"{_gst_bool(self.params.get('anc_split_by_packet'))}",
                ],
            ),
        )
        # ``timeout`` is left at its 60s default: it is a per-frame budget in
        # gst_mtl_st40p_rx.c, not a run length. Frame geometry is auto-detected.
        rx_cmd = self._pipeline(
            self._mtl_element(
                "st40p",
                is_tx=False,
                nic_port_list=nic_port_list,
                extra=[
                    "output-format=raw-udw",
                    f"rx-interlaced={_gst_bool(self.params.get('interlaced'))}",
                ],
            ),
            "filesink location={out}",
        )
        return tx_cmd, rx_cmd

    # -- pipeline fragments ----------------------------------------------
    def _pipeline(self, *stages: str) -> str:
        """Join pipeline stages into a full ``gst-launch-1.0`` command line."""
        return " ! ".join((f"{self.get_executable_path()} {stages[0]}",) + stages[1:])

    def _looping_source(self) -> str:
        """TX file source that replays the asset for the whole window.

        ``multifilesrc`` with a single location and ``loop=true`` is GStreamer's
        equivalent of FFmpeg's ``-stream_loop -1``: one element, and it re-emits
        the file indefinitely so the run length is decided by when the harness
        stops the pipeline rather than by the asset's duration.
        """
        return f"multifilesrc location={self.params['input_file']} loop=true"

    def _mtl_element(
        self,
        session_type: str,
        *,
        is_tx: bool,
        nic_port_list,
        extra: Optional[list[str]] = None,
    ) -> str:
        """One MTL element with its network properties and session extras.

        TX egresses ``nic_port_list[0]`` and RX ingresses ``nic_port_list[1]``,
        matching RxTxApp and FFmpeg: the ``pcap_capture`` fixture sniffs the
        second NIC's PF, so TX has to leave through the other one or the capture
        only ever sees switch-batched packets.
        """
        tx_element, rx_element = _ELEMENTS[session_type]
        dst_ip = self._dst_ip()
        is_multicast = ipaddress.ip_address(dst_ip).is_multicast
        props = [
            f"dev-port={nic_port_list[0] if is_tx else nic_port_list[1]}",
            f"dev-ip={ip_pools.tx[0] if is_tx else ip_pools.rx[0]}",
            # TX ``ip`` is the destination; RX ``ip`` is the multicast group to
            # join, or the sender's address for unicast
            # (gst_mtl_common_parse_{tx,rx}_port_arguments).
            f"ip={dst_ip if is_tx or is_multicast else ip_pools.tx[0]}",
            f"udp-port={self.params['port']}",
            f"payload-type={self.params['payload_type']}",
        ]
        queues = self.params.get("tx_queues_cnt" if is_tx else "rx_queues_cnt")
        if queues:
            props.append(f"{'tx' if is_tx else 'rx'}-queues={queues}")
        framebuffers = self.params.get("framebuffer_count")
        if framebuffers:
            # st40p spells the property ``-cnt``, st20p/st30p ``-num``.
            suffix = "cnt" if session_type == "st40p" else "num"
            props.append(f"{'tx' if is_tx else 'rx'}-framebuff-{suffix}={framebuffers}")
        if self.params.get("enable_ptp"):
            # Each pipeline runs its own MTL instance, so both need the flag.
            props.append("enable-ptp=true")
        return " ".join([tx_element if is_tx else rx_element] + props + (extra or []))

    def _dst_ip(self) -> str:
        """Destination address the TX side sends to.

        An explicit ``destination_ip`` / ``multicast_ip`` wins; otherwise
        ``test_mode`` picks the multicast group or the RX unicast address, the
        same convention the other adapters follow.
        """
        explicit = self.params.get("destination_ip") or self.params.get("multicast_ip")
        if explicit:
            return explicit
        if self.params.get("test_mode") == "multicast":
            return ip_pools.rx_multicast[0]
        return ip_pools.rx[0]

    # --------------------------------------------------- prepare_execution
    def prepare_execution(self, build: str, host=None, **kwargs):
        """Resolve the RX output path and check the plugin is loadable."""
        if not host:
            raise ValueError("host required for GStreamer execution")
        self._build = build
        self._plugin_path = ":".join(f"{build}/{d}" for d in _PLUGIN_DIRS)

        out_file = self.params.get("output_file")
        if not out_file:
            # st40p tests assert on the wire, not on a golden file, so they pass
            # no output_file -- but RX still needs a sink whose size proves data
            # arrived. Park it next to the input and clean it up afterwards.
            session_type = self.params["session_type"]
            input_path = host.connection.path(self.params["input_file"])
            out_file = str(input_path.parent / f"gst_{session_type}_rx.out")
            self.params["output_file"] = out_file
        self._output_files = [out_file]
        host.connection.path(out_file).touch()
        self.command = self.command.replace("{out}", out_file)

        self._require_plugin(host)

    def _require_plugin(self, host) -> None:
        """Raise ``EnvironmentError`` when the MTL plugin is not installed.

        Without this the pipeline fails with ``no element "mtl_st20p_tx"`` and
        ``gst-launch-1.0`` exits 1, which reads like a product bug; a missing
        plugin is an environment problem (see the setup script) and must be
        reported as one.
        """
        elements = _ELEMENTS[self.params["session_type"]]
        # gst-inspect-1.0 only reports on its last argument, so chain one call
        # per element rather than passing both at once.
        result = host.connection.execute_command(
            " && ".join(
                f"{_GST_INSPECT} --gst-plugin-path={self._plugin_path} {element}"
                for element in elements
            ),
            shell=True,
            timeout=30,
            expected_return_codes=None,
        )
        if result.return_code != 0:
            raise EnvironmentError(
                f"MTL GStreamer elements {elements} not found under "
                f"{self._plugin_path}; build the plugin with "
                f"'.github/scripts/acceptance_setup.sh setup --base-only'"
            )

    # ----------------------------------------------------- execute_test
    def execute_test(  # type: ignore[override]
        self,
        build: str,
        test_time: int = 30,
        host=None,
        sleep_interval: int = 4,
        compliance=NO_COMPLIANCE,
        integrity=NO_INTEGRITY,
        interface_setup=None,
        fail_on_error: bool = True,
        **extra,
    ) -> bool:
        """Single-host RX-then-TX orchestrator.

        RX starts first because ``mtl_st20p_rx``/``mtl_st30p_rx`` have to be
        listening before the sender's pacing settles. Neither pipeline
        self-terminates (the source loops), so the wall-clock ``test_time``
        bounds the run and the base helper stops both.

        Dual-host orchestration is not supported: both pipelines share one
        host's DPDK process group. Passing those keys is a caller bug.
        """
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

        # Recorded before prepare_execution so validation and capture_intent
        # both size themselves against the window the caller asked for.
        self.params["test_time"] = test_time
        self.prepare_execution(build=build, host=host, interface_setup=interface_setup)
        # PTP sync happens before any frame moves, so the data window has to be
        # extended by it. gst pipelines carry no ``--test_time`` to rewrite.
        effective_test_time, _ = self._apply_ptp_extension(test_time)

        specs = [
            ProcSpec(
                cmd=f"{self.command} --gst-plugin-path={self._plugin_path}",
                host=host,
                label="RX",
                bounded=False,
                graceful_s=_STOP_GRACEFUL_S,
            ),
            ProcSpec(
                cmd=f"{self._tx_commands[0]} --gst-plugin-path={self._plugin_path}",
                host=host,
                label="TX",
                bounded=False,
                graceful_s=_STOP_GRACEFUL_S,
            ),
        ]

        # Traffic only flows once TX is up, so the capture is armed after the
        # last process starts (as in the FFmpeg adapter) rather than the first.
        intent = self.capture_intent()
        self._run_proc_group(
            specs,
            build=build,
            test_time=effective_test_time,
            sleep_interval=sleep_interval,
            wall_clock_seconds=effective_test_time,
            cleanup_host=host,
            after_last_start=lambda _proc: compliance.arm(intent),
        )
        self._results = [
            {
                "label": spec.label,
                "return_code": self._safe_return_code(spec.proc),
                "output": spec.captured_output or "",
            }
            for spec in specs
        ]
        # Both pipelines emit MTL stats, so the combined text is what log
        # scanners and ``count_tx_dropped_frames()`` need to see.
        self.last_output = "\n".join(
            f"--- {r['label']} ---\n{r['output']}" for r in self._results
        )
        self.last_return_code = self._results[0]["return_code"]

        return self._finalize_run(
            compliance,
            intent,
            fail_on_error,
            integrity=integrity,
            integrity_intent=self.integrity_intent(build, host),
        )

    # ----------------------------------------------------- compliance
    def _resolve_capture_dst_ip(self):
        """Destination IP netsniff filters on -- the address TX transmits to."""
        return self._dst_ip()

    # ----------------------------------------------------- validate
    def validate_results(self, fail_on_error: bool = True) -> bool:  # type: ignore[override]
        if not self._results:
            self._fail_validation(
                "GStreamer validate_results called before execute_test",
                fail_on_error,
            )
            return False

        problems: list[str] = []
        for result in self._results:
            problems += self._check_pipeline_exit(result)
            problems += self._check_pipeline_frames(result)
        problems += self._check_rx_output()

        # Deleting the RX dump is safe here: _finalize_run evaluates integrity
        # before validate_results precisely so this can reclaim ramdisk space.
        self._cleanup_output_files(self._host)

        if problems:
            self._fail_validation(
                "GStreamer pipeline check failed: " + "; ".join(problems),
                fail_on_error,
            )
            return False
        return True

    def _check_pipeline_exit(self, result: dict) -> list[str]:
        """Return code and error-text checks for one pipeline."""
        label, code, output = result["label"], result["return_code"], result["output"]
        problems = []
        # ``None`` and -9/137 both mean our own stop ladder ran out of patience
        # and used SIGKILL -- see _stop_unbounded_proc, which documents both the
        # SIGKILL leg and the unreadable return code that can follow it.
        # _STOP_GRACEFUL_S is measured on one session pair, so a larger geometry
        # can legitimately exceed it; failing on that would report a harness
        # timing artifact as a product bug. What the run actually produced is
        # bounded by the oracles below instead.
        if code in (None, -9, 137):
            logger.warning(
                "[GStreamer] %s pipeline was SIGKILLed after %ds of grace "
                "(return code %s); relying on the data oracles for this run",
                label,
                _STOP_GRACEFUL_S,
                code,
            )
        elif code != 0:
            problems.append(f"{label} pipeline exited with {code}")
        # ``ERROR:`` prefixes gst-launch's bus errors ("ERROR: from element ...",
        # "ERROR: pipeline doesn't want to preroll") and its parse failures --
        # the authoritative "this pipeline did not work" signal. Element-internal
        # GST_ERROR lines (the debug log's " ERROR " column) are surfaced for
        # triage but do not fail on their own: they are advisory unless the
        # element also fails the pipeline, which the checks above catch.
        errors = [
            line.strip()
            for line in output.splitlines()
            if "ERROR:" in line or "erroneous pipeline" in line
        ]
        if errors:
            problems.append(
                f"{label} pipeline reported {len(errors)} error(s), first: {errors[0]}"
            )
        for line in output.splitlines():
            if " ERROR " in line:
                logger.warning("[GStreamer] %s element error: %s", label, line.strip())
        return problems

    def _check_pipeline_frames(self, result: dict) -> list[str]:
        """MTL must report frames moved in the direction this pipeline drives."""
        direction = result["label"]
        series = self.pipeline_frame_series(result["output"], direction)
        if self._expected_rx_bytes() is None and len(series) < _MIN_GRADED_INTERVALS:
            # Where _check_rx_output has no byte count to work with -- st40p,
            # whose payload size per frame is the sender's choice -- these
            # counters are the only throughput evidence there is, so too few of
            # them has to fail rather than log. Passing here would leave "the
            # process exited 0" as the entire oracle, which is the false
            # positive this whole path exists to remove, and it would grade a
            # 20s run (one interval) more weakly than a 15s one (zero
            # intervals, already a failure before this change).
            #
            # The condition is written against the byte count rather than
            # against session_type=="st40p" because the dependency really is
            # "no byte oracle => stats are mandatory"; that stays right if a
            # session type is added or if _expected_rx_bytes loses a case.
            return [
                f"MTL printed {len(series)} {direction} pipeline stats "
                f"interval(s) for session_type="
                f"{self.params.get('session_type')}, which has no expected byte "
                f"count; {_MIN_GRADED_INTERVALS} are needed to grade a rate. "
                f"Either test_time={self.params.get('test_time')} is under the "
                f"{(_MIN_GRADED_INTERVALS + 1) * _MTL_STATS_INTERVAL_S}s that "
                f"takes, or the session stopped early"
            ]
        if not series:
            logger.info(
                "[GStreamer] no MTL %s pipeline stats in this run "
                "(shorter than one stats interval)",
                direction,
            )
            return []
        frames = sum(series)
        if frames == 0:
            return [f"MTL reported 0 {direction} frames"]
        # A frame floor, not just frames>0: the stale-wake regression this suite
        # has to catch produced clean-exiting runs at ~21% of nominal, which
        # frames>0 passes. st40p has no byte rate, so its floor has to be counted
        # in frames or it has no throughput oracle at all.
        #
        # st20p/st30p keep _check_rx_output's byte floor instead. That floor is
        # the *looser* of the two -- _MIN_CAPTURE_RATIO spans the whole window
        # including startup, so it tolerates a lower sustained rate than
        # _MIN_STEADY_FRAME_RATIO does -- but those session types also have
        # per-payload oracles the ancillary ones lack (integrity and compliance
        # fixtures), and adding a second throughput check here would mean one
        # more way for a healthy run to fail without closing a gap.
        if self._expected_rx_bytes() is not None:
            logger.info("[GStreamer] MTL reported %d %s frames", frames, direction)
            return []
        # st30p also lands here when _AUDIO_SAMPLE_BYTES has no entry for its
        # format: no byte rate, and no framerate to grade a frame rate against
        # either, so the count is reported and left ungraded.
        framerate = self.params.get("framerate")
        if framerate is None:
            logger.info("[GStreamer] MTL reported %d %s frames", frames, direction)
            return []
        # Grade the steady state: the first printed interval overlaps startup --
        # RX is up and counting before TX sends its first packet -- so it is not
        # a rate sample. Measured on a healthy p59 run, the per-interval series
        # was RX [246, 600] and TX [504, 599]: the second interval is the real
        # rate (599/590 nominal) while including the first would drag the total
        # to 72%. Dropping it makes the floor independent of how long startup
        # took, rather than needing enough slack to absorb it. Non-empty by the
        # _MIN_GRADED_INTERVALS check above, which every caller reaching here has
        # passed.
        steady = series[1:]
        # extract_framerate truncates the MTL token (p59 -> 59 against a real
        # 59.94), erring toward a lower floor, which is the right direction.
        expected = (
            self.extract_framerate(framerate) * len(steady) * _MTL_STATS_INTERVAL_S
        )
        minimum = int(expected * _MIN_STEADY_FRAME_RATIO)
        logger.info(
            "[GStreamer] MTL %s frame series %s; steady-state %d frames over "
            "%ds (nominal %d, minimum %d)",
            direction,
            series,
            sum(steady),
            len(steady) * _MTL_STATS_INTERVAL_S,
            expected,
            minimum,
        )
        if sum(steady) < minimum:
            return [
                f"MTL moved {sum(steady)} {direction} frames in the "
                f"{len(steady) * _MTL_STATS_INTERVAL_S}s after startup, under "
                f"{minimum} ({_MIN_STEADY_FRAME_RATIO:.0%} of the {expected} that "
                f"framerate={framerate} implies); per-interval series {series}"
            ]
        return []

    def _check_rx_output(self) -> list[str]:
        """RX dump must exist and be large enough for the requested window."""
        out_file = self._output_files[0] if self._output_files else None
        if not out_file or self._host is None:
            return ["no RX output file recorded"]
        result = self._host.connection.execute_command(
            f"stat -c %s {out_file}", shell=True, expected_return_codes=None
        )
        if result.return_code != 0:
            return [f"RX output file missing: {out_file}"]
        size = int((result.stdout or "0").strip() or 0)
        if size == 0:
            return [f"RX output file is empty: {out_file}"]
        # Sized against the requested ``test_time``, never the PTP-extended
        # wall clock: no frame moves during PTP sync, so charging the capture
        # for those seconds would fail a healthy run whose lock took a while.
        # A run that gets its full window plus leftover sync time can exceed
        # 100% here, which is fine -- this is a floor, not a target.
        expected = self._expected_rx_bytes()
        if expected:
            minimum = int(expected * _MIN_CAPTURE_RATIO)
            logger.info(
                "[GStreamer] RX captured %d bytes (nominal %d, minimum %d)",
                size,
                expected,
                minimum,
            )
            if size < minimum:
                return [
                    f"RX captured {size} bytes, under {minimum} "
                    f"({_MIN_CAPTURE_RATIO:.0%} of the {expected} expected for "
                    f"{self.params.get('test_time')}s) -- the stream did not run "
                    f"for the test window"
                ]
        return []

    def _expected_rx_bytes(self) -> Optional[int]:
        """Bytes a full-length RX capture holds, or ``None`` when not exact.

        Only computed where the byte rate follows from the session parameters
        alone. st40p is excluded on purpose: its raw-UDW payload size per frame
        is decided by the sender's ancillary content, so any bound here would be
        a guess, and its frame delivery is covered by the MTL stats check.
        """
        test_time = self.params.get("test_time") or 0
        session_type = self.params.get("session_type")
        if not test_time:
            return None
        if session_type == "st20p":
            frame_size = _st20p_frame_size(
                self.params["pixel_format"],
                int(self.params["width"]),
                int(self.params["height"]),
            )
            return (
                frame_size
                * self.extract_framerate(self.params["framerate"])
                * test_time
            )
        if session_type == "st30p":
            sample_bytes = _AUDIO_SAMPLE_BYTES.get(self.params["audio_format"])
            if not sample_bytes:
                return None
            return (
                audio_sampling_hz(self.params["audio_sampling"])
                * audio_channel_count(self.params["audio_channels"])
                * sample_bytes
                * test_time
            )
        return None


def _st20p_frame_size(pixel_format: str, width: int, height: int) -> int:
    """Bytes one st20p frame occupies in the RX dump."""
    if pixel_format == "v210":
        # 6 pixels per 16 bytes, each row padded to a 48-pixel group.
        return ((width + 47) // 48) * 128 * height
    # YUV422PLANAR10LE: 2 bytes per component, 2 components per pixel.
    return width * height * 4


def _gst_bool(value) -> str:
    return "true" if value else "false"
