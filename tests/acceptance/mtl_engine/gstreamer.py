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
     3. The ST20 RX artifact contains one complete frame for integrity; ST30
         contains at least :data:`_MIN_CAPTURE_RATIO` of its full-window bytes.
     4. For ST20 and ST40, MTL's per-interval frame counts prove throughput: the
         RX side must report enough intervals to grade, and every graded
       direction must hold :data:`_MIN_STEADY_FRAME_RATIO` of the rate the
       session was configured for -- otherwise "one frame moved" would be
       st40p's whole throughput oracle. Only RX is required to have gradeable
       intervals; see :meth:`GStreamer._check_pipeline_frames` for why TX is
       graded when it can be but never required to be.

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

# Minimum fraction of a full-length audio capture the RX side must produce.
# ST20 keeps one complete frame for integrity and uses MTL frame counters to
# prove full-window throughput without filling the media ramdisk.
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
            "multifilesink location={out} max-files=1",
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
        rate_checked_session = self.params.get("session_type") in ("st20p", "st40p")
        problems: list[str] = []
        judged: list[str] = []  # directions a throughput verdict was reached for
        for result in self._results:
            direction = result["label"]
            series = self.pipeline_frame_series(result["output"], direction)
            if not series:
                logger.info(
                    "[GStreamer] no MTL %s pipeline stats in this run "
                    "(shorter than one stats interval)",
                    direction,
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
            problems.append(
                f"MTL printed no gradeable {_RX_LABEL} stats for session_type="
                f"{self.params.get('session_type')}, whose throughput these "
                f"counters are the only evidence of; {_MIN_GRADED_INTERVALS} "
                f"completed intervals need "
                f"{_MIN_GRADED_INTERVALS * _MTL_STATS_INTERVAL_S}s of pipeline "
                f"uptime plus initialization; test_time="
                f"{self.params.get('test_time')} was insufficient or the "
                f"session stopped early"
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
        # Sized against the requested ``test_time``, never the PTP-extended
        # wall clock: no frame moves during PTP sync, so charging the capture
        # for those seconds would fail a healthy run whose lock took a while.
        # A run that gets its full window plus leftover sync time can exceed
        # 100% here, which is fine -- this is a floor, not a target.
        expected = self._expected_rx_bytes()
        if expected:
            minimum = (
                expected
                if self.params.get("session_type") == "st20p"
                else int(expected * _MIN_CAPTURE_RATIO)
            )
            logger.info(
                "[GStreamer] RX captured %d bytes (nominal %d, minimum %d)",
                size,
                expected,
                minimum,
            )
            if size < minimum:
                if self.params.get("session_type") == "st20p":
                    return [
                        f"RX captured {size} bytes, under one complete "
                        f"{expected}-byte video frame"
                    ]
                return [
                    f"RX captured {size} bytes, under {minimum} "
                    f"({_MIN_CAPTURE_RATIO:.0%} of the {expected} expected for "
                    f"{self.params.get('test_time')}s) -- the stream did not run "
                    f"for the test window"
                ]
        return []

    def _expected_rx_bytes(self) -> Optional[int]:
        """Bytes required in the RX artifact, or ``None`` when not exact.

        ST20 retains one complete frame for integrity while frame counters prove
        throughput. ST30 retains the duration-sized byte oracle. ST40 is
        excluded because its raw-UDW payload size is sender-defined.
        """
        test_time = self.params.get("test_time") or 0
        session_type = self.params.get("session_type")
        if not test_time:
            return None
        if session_type == "st20p":
            return _st20p_frame_size(
                self.params["pixel_format"],
                int(self.params["width"]),
                int(self.params["height"]),
            )
        if session_type == "st30p":
            return (
                audio_sampling_hz(self.params["audio_sampling"])
                * audio_channel_count(self.params["audio_channels"])
                * _AUDIO_SAMPLE_BYTES[self.params["audio_format"]]
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
