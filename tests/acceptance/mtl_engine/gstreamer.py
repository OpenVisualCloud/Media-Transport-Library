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
    1. Every ``gst-launch-1.0`` exited cleanly after the harness sent SIGINT.
       A process that requires SIGKILL is a failure.
    2. No ``ERROR:`` / ``erroneous pipeline`` line in either pipeline's output.
    3. The ST20 and ST30 RX artifacts each hold at least
       :data:`_MIN_CAPTURE_RATIO` of their full-window bytes, so the MD5
       integrity check grades the whole dump exactly as it does for the RxTxApp
       and FFmpeg adapters -- neither of which bounds its RX dump either.
    4. For ST20 and ST40, MTL's per-interval frame counts prove throughput: the
       RX side must report enough intervals to grade, and every graded
       direction must hold :data:`_MIN_STEADY_FRAME_RATIO` of the rate the
       session was configured for -- otherwise "one frame moved" would be
       st40p's whole throughput oracle. Only RX is required to have gradeable
       intervals; see :meth:`GStreamer._check_pipeline_frames` for why TX is
       graded when it can be but never required to be.

Both (3) and (4) grade a duration, so every session extends its wall clock to
:data:`_MIN_GRADED_WALL_CLOCK_S` rather than relax an oracle that a shorter
window cannot satisfy -- see :meth:`GStreamer._graded_wall_clock`.

Compliance (EBU) and integrity (MD5) are evaluated by the base class through
``capture_intent`` / ``integrity_intent`` exactly as for the other adapters --
this adapter adds no private validation path.
"""

from __future__ import annotations

import ipaddress
import logging
from typing import Optional

from common.integrity.video_integrity import calculate_yuv_frame_size
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

# Minimum fraction of a full-length capture the RX side must produce, for both
# the ST20 video dump and the ST30 audio dump. What it adds depends on the
# session type: for st20p it is a second, independent bound on average
# throughput alongside the per-interval frame counters, which are what catch a
# session that delivered the right total unevenly; for st30p, absent from
# ``_RATE_CHECKED_SESSIONS``, it is the *only* throughput oracle there is, so it
# must not be lowered without rate-checking st30p as well.
_MIN_CAPTURE_RATIO = 0.5

# Minimum fraction of nominal frames required in each completed interval after
# startup. Healthy p29/p50/p59 runs met nominal; 0.9 leaves jitter margin while
# rejecting the measured half-rate and stale-wake failures.
_MIN_STEADY_FRAME_RATIO = 0.9

# MTL prints and resets pipeline counters after each completed 10s interval; it
# does not print a partial interval during teardown. RX starts first, so TX can
# legitimately have one fewer sample.
_MTL_STATS_INTERVAL_S = 10

# Intervals needed before a rate can be graded: one to discard because it
# overlaps process startup, one to measure.
_MIN_GRADED_INTERVALS = 2

# Session types whose throughput verdict comes from the MTL frame counters, so
# the run has to be long enough for one interval to be gradeable. st40p has no
# byte oracle at all (``_expected_rx_bytes`` returns None for it), which is why
# a short window is lengthened rather than excused: excusing it would leave "the
# process exited 0" as st40p's entire throughput check.
_RATE_CHECKED_SESSIONS = ("st20p", "st40p")

# Shortest wall clock any session may run for. Two things need it:
#
# * The frame-rate oracle needs _MIN_GRADED_INTERVALS completed stats lines. MTL
#   arms the stats alarm inside ``mtl_init`` -- ``rte_eal_alarm_set`` in
#   mt_stat_init (lib/src/mt_stat.c), reached from mt_main.c *after* rte_eal_init
#   -- so the first line lands one whole interval after that init completes,
#   never at process start. Two intervals to grade one, plus a third interval's
#   worth of allowance for gst-launch startup and EAL init. Measured on an E810
#   host: test_time=20 printed one RX interval, test_time=30 printed two.
# * The byte floors are a fraction of framerate x window, but the window is
#   measured from the RX start while TX only starts ``sleep_interval`` (4s) later
#   and then pays its own gst + EAL init. That fixed cost is a minority of a 30s
#   window and a majority of a 10s one, so a short window fails st30p -- which is
#   not rate-checked -- on the byte floor alone.
_MIN_GRADED_WALL_CLOCK_S = (_MIN_GRADED_INTERVALS + 1) * _MTL_STATS_INTERVAL_S

# ProcSpec labels, also the direction tokens MTL prints in its stats lines, so
# ``pipeline_frame_series`` can select a direction by the label that produced it.
_RX_LABEL = "RX"
_TX_LABEL = "TX"

# Seconds to let a pipeline finish on SIGINT before the base class escalates.
# ST30P teardown measured 21.7s, so the universal 10s default is insufficient.
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
        # Wall clock the pipelines actually ran for, which is the requested
        # ``test_time`` plus any PTP extension and rate-check floor. Only the
        # frame-counter diagnostic reads it; the oracles stay sized against the
        # requested window.
        self._wall_clock_s: Optional[int] = None

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
        session_type = params.get("session_type")
        if session_type is not None and session_type not in _ELEMENTS:
            return (
                f"MTL GStreamer plugin supports {sorted(_ELEMENTS)}, "
                f"not session_type={session_type}"
            )
        if params.get("enable_rtcp"):
            # gst_mtl_common.c installs no RTCP property on any MTL element, so
            # there is no way to ask the plugin for an RTCP-enabled session.
            return "MTL GStreamer plugin exposes no RTCP property (gst_mtl_common.c)"
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
            if params.get("audio_sampling") == "44.1kHz":
                return "MTL ST30 does not define a packet time for 44.1 kHz audio"
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
            # Unbounded, like RxTxApp (``rx_max_file_size`` defaults to 0) and
            # the FFmpeg adapter: the whole window has to reach disk for the
            # byte oracle and the MD5 integrity check to mean anything. The
            # media ramdisk is sized for it -- see ``_media_ramdisk_gib`` in
            # tests/acceptance/configs/gen_config.py.
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
        self-terminates (the source loops), so a wall clock bounds the run and
        the base helper stops both. That wall clock is ``test_time`` extended by
        PTP sync and then raised to :data:`_MIN_GRADED_WALL_CLOCK_S` -- it is
        what the run lasts, while ``self.params["test_time"]`` keeps the
        requested value for the oracles sized against it.

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
        effective_test_time = self._graded_wall_clock(effective_test_time)
        self._wall_clock_s = effective_test_time

        specs = [
            ProcSpec(
                cmd=f"{self.command} --gst-plugin-path={self._plugin_path}",
                host=host,
                label=_RX_LABEL,
                bounded=False,
                graceful_s=_STOP_GRACEFUL_S,
            ),
            ProcSpec(
                cmd=f"{self._tx_commands[0]} --gst-plugin-path={self._plugin_path}",
                host=host,
                label=_TX_LABEL,
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
                "exited_before_stop": spec.exited_before_stop,
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

    def _graded_wall_clock(self, effective_test_time: int) -> int:
        """Raise the wall clock to the shortest window the oracles can grade.

        Below :data:`_MIN_GRADED_WALL_CLOCK_S` a healthy run fails: st20p/st40p
        cannot print :data:`_MIN_GRADED_INTERVALS` stats lines for
        :meth:`_check_pipeline_frames`, and every session's byte floor charges
        for a window whose fixed startup cost it cannot amortise. Extending the
        wall clock is the right side to give on -- st40p has no byte-count oracle
        to fall back on (``_expected_rx_bytes`` returns None for it), so relaxing
        the requirement instead would turn a false fail into a silent false pass.

        Only the returned wall clock moves; ``self.params["test_time"]`` keeps
        the requested value, so ``capture_intent()`` stays sized against the
        window the caller asked for -- the compliance capture is a fixed-length
        sample of the stream, not a measurement of its total.
        :meth:`_expected_rx_bytes` does follow this extension, because the RX
        dump grows for all of it, and so does the media tmpfs
        ``configs/gen_config.py`` sizes for the same reason.
        """
        if effective_test_time >= _MIN_GRADED_WALL_CLOCK_S:
            return effective_test_time
        logger.warning(
            "[GStreamer] extending the %s run from the configured %ds to %ds: "
            "the oracles need %d completed %ds MTL stats intervals and a window "
            "long enough to absorb pipeline startup, and a shorter one fails a "
            "healthy session. Raise test_config.yaml::test_time to at least %d "
            "to avoid this.",
            self.params.get("session_type"),
            effective_test_time,
            _MIN_GRADED_WALL_CLOCK_S,
            _MIN_GRADED_INTERVALS,
            _MTL_STATS_INTERVAL_S,
            _MIN_GRADED_WALL_CLOCK_S,
        )
        return _MIN_GRADED_WALL_CLOCK_S

    # ----------------------------------------------------- compliance
    def _resolve_capture_dst_ips(self) -> tuple[str, ...]:
        """Destination IP netsniff filters on -- the address TX transmits to."""
        return (self._dst_ip(),)

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
        problems += self._check_pipeline_frames()
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
        if result.get("exited_before_stop") is None:
            problems.append(
                f"{label} pipeline liveness could not be verified at teardown"
            )
        elif result["exited_before_stop"]:
            problems.append(f"{label} pipeline exited before the test window ended")
        if code in (None, -9, 137):
            problems.append(
                f"{label} pipeline did not exit cleanly after "
                f"{_STOP_GRACEFUL_S}s of grace (return code {code})"
            )
        elif code != 0:
            problems.append(f"{label} pipeline exited with {code}")
        errors = [
            line.strip()
            for line in output.splitlines()
            if "ERROR:" in line or "erroneous pipeline" in line
        ]
        if errors:
            problems.append(
                f"{label} pipeline reported {len(errors)} error(s), first: {errors[0]}"
            )
        element_errors = [
            line.strip() for line in output.splitlines() if " ERROR " in line
        ]
        if element_errors:
            problems.append(
                f"{label} pipeline reported {len(element_errors)} element error(s), "
                f"first: {element_errors[0]}"
            )
        return problems

    def _check_pipeline_frames(self) -> list[str]:
        """Grade MTL frame counters where they are the throughput oracle.

        RX must provide enough intervals because it runs for the full window;
        TX starts later and may legitimately provide one fewer sample.
        """
        rate_checked_session = self.params.get("session_type") in _RATE_CHECKED_SESSIONS
        problems: list[str] = []
        judged: list[str] = []  # directions a throughput verdict was reached for
        for result in self._results:
            direction = result["label"]
            series = self.pipeline_frame_series(result["output"], direction)
            if not series:
                logger.warning(
                    "[GStreamer] no MTL %s pipeline stats in this run, over a "
                    "%ss wall clock floored at %ss to fit %d intervals: the "
                    "session printed no counter line at all",
                    direction,
                    self._wall_clock_s,
                    _MIN_GRADED_WALL_CLOCK_S,
                    _MIN_GRADED_INTERVALS,
                )
                continue
            if sum(series) == 0:
                if direction == _RX_LABEL or len(series) >= _MIN_GRADED_INTERVALS:
                    problems.append(f"MTL reported 0 {direction} frames")
                    judged.append(direction)
                continue
            logger.info(
                "[GStreamer] MTL %s frame series %s (%d frames)",
                direction,
                series,
                sum(series),
            )
            if not rate_checked_session or len(series) < _MIN_GRADED_INTERVALS:
                continue
            judged.append(direction)
            problems += self._check_steady_frame_rate(direction, series)
        if rate_checked_session and _RX_LABEL not in judged:
            # st40p is in _RATE_CHECKED_SESSIONS but has no byte oracle at all
            # -- _expected_rx_bytes() returns None for it, so _check_rx_output
            # only checks the dump exists. Saying "only bounds the average"
            # there would tell the operator the average throughput was still
            # verified when nothing was.
            try:
                bounded = self._expected_rx_bytes() is not None
            except (KeyError, ValueError):
                # A geometry the size table does not know is also a geometry no
                # byte floor can be derived from, so it bounds nothing either.
                bounded = False
            fallback = (
                "_check_rx_output bounds only the average"
                if bounded
                else "there is no byte-count oracle for this session type"
            )
            problems.append(
                f"MTL printed no gradeable {_RX_LABEL} stats for session_type="
                f"{self.params.get('session_type')}, whose per-interval "
                f"steadiness these counters are the only evidence of "
                f"({fallback}); "
                f"{_MIN_GRADED_INTERVALS} completed intervals need "
                f"{_MIN_GRADED_INTERVALS * _MTL_STATS_INTERVAL_S}s of pipeline "
                f"uptime plus initialization, and the pipelines ran for "
                f"{self._wall_clock_s}s -- the session stopped early or "
                f"initialization overran that budget"
            )
        return problems

    def _check_steady_frame_rate(self, direction: str, series: list[int]) -> list[str]:
        """Grade each completed interval after the partial startup interval."""
        steady = series[1:]
        framerate = self.params["framerate"]
        # p59 is truncated to 59 rather than 59.94, producing a conservative floor.
        nominal = self.extract_framerate(framerate) * _MTL_STATS_INTERVAL_S
        minimum = int(nominal * _MIN_STEADY_FRAME_RATIO)
        logger.info(
            "[GStreamer] %s steady-state intervals %s (nominal %d each, minimum %d)",
            direction,
            steady,
            nominal,
            minimum,
        )
        starved = [frames for frames in steady if frames < minimum]
        if starved:
            return [
                f"MTL moved {min(starved)} {direction} frames in a "
                f"{_MTL_STATS_INTERVAL_S}s interval after startup, under {minimum} "
                f"({_MIN_STEADY_FRAME_RATIO:.0%} of the {nominal} that "
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
        # Sized against the seconds frames actually move in -- see
        # :meth:`_expected_rx_bytes`, which follows :meth:`_graded_wall_clock`'s
        # extension but not the PTP one. A run that gets its full window can
        # exceed 100% here, which is fine -- this is a floor, not a target.
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
                    f"({_MIN_CAPTURE_RATIO:.0%} of the {expected} expected for the "
                    f"streaming window) -- the stream did not run for the test "
                    f"window"
                ]
        return []

    def _expected_rx_bytes(self) -> Optional[int]:
        """Bytes required in the RX artifact, or ``None`` when not exact.

        ST20 and ST30 are both duration-sized -- frame_size x framerate x
        window and sample_rate x channels x sample_bytes x window. ST40 is
        excluded because its raw-UDW payload size is sender-defined.
        """
        test_time = self.params.get("test_time") or 0
        session_type = self.params.get("session_type")
        if not test_time:
            return None
        # The window is the span frames actually move in, which is not always the
        # requested test_time. :meth:`_graded_wall_clock` lengthens a short run
        # to _MIN_GRADED_WALL_CLOCK_S and frames move for every second of that
        # extension, so a floor left on the requested value is a fraction of a
        # fraction: at test_time=10 it demands 5s of data from a 30s run that
        # delivers ~24s. For st30p, absent from _RATE_CHECKED_SESSIONS, that
        # floor is the entire throughput verdict, so a session that stalls after
        # 5s would still pass.
        #
        # The PTP extension is the opposite case and stays excluded: no frame
        # moves during sync, so charging the capture for those seconds would fail
        # a healthy run whose lock took a while. Hence the graded minimum less the
        # dead sync seconds -- which is how long _graded_wall_clock's own output,
        # max(test_time + ptp_dead, _MIN_GRADED_WALL_CLOCK_S), spends streaming.
        # Whenever the requested window already reaches the minimum on its own,
        # this is just test_time.
        ptp_dead = (
            self.params.get("ptp_sync_time", 50)
            if self.params.get("enable_ptp", False)
            else 0
        )
        window = max(test_time, _MIN_GRADED_WALL_CLOCK_S - ptp_dead)
        if session_type == "st20p":
            # extract_framerate truncates p59 to 59 and p119 to 119, so the
            # product is a conservative floor rather than an exact target.
            return (
                _st20p_frame_size(
                    self.params["pixel_format"],
                    int(self.params["width"]),
                    int(self.params["height"]),
                )
                * self.extract_framerate(self.params["framerate"])
                * window
            )
        if session_type == "st30p":
            return (
                audio_sampling_hz(self.params["audio_sampling"])
                * audio_channel_count(self.params["audio_channels"])
                * _AUDIO_SAMPLE_BYTES[self.params["audio_format"]]
                * window
            )
        return None


def _st20p_frame_size(pixel_format: str, width: int, height: int) -> int:
    """Bytes one st20p frame occupies in the RX dump.

    Delegates to the same table the MD5 integrity check chunks the dump with
    (``common.integrity.video_integrity``), so the byte oracle and the integrity
    verdict can never disagree about where a frame boundary is.
    """
    return calculate_yuv_frame_size(width, height, pixel_format)


def _gst_bool(value) -> str:
    return "true" if value else "false"
