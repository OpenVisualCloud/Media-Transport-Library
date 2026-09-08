# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

from types import SimpleNamespace

import pytest
from mtl_engine import ip_pools
from mtl_engine.application_base import ProcSpec
from mtl_engine.config.mappings import GSTREAMER_AUDIO_FORMAT_MAP
from mtl_engine.ffmpeg import FFmpeg
from mtl_engine.gstreamer import (
    _AUDIO_SAMPLE_BYTES,
    _MIN_GRADED_WALL_CLOCK_S,
    GStreamer,
)


def _stats(direction: str, frames: list[int]) -> str:
    return "\n".join(
        f"{direction}_st40p(0), frame get try {count} succ {count}, put {count}"
        for count in frames
    )


def _app(rx: list[int], tx: list[int]) -> GStreamer:
    app = GStreamer()
    app.params = {"session_type": "st40p", "framerate": "p59", "test_time": 30}
    app._wall_clock_s = 30
    app._results = [
        {"label": "RX", "output": _stats("RX", rx)},
        {"label": "TX", "output": _stats("TX", tx)},
    ]
    return app


def test_frame_oracle_accepts_healthy_rx_with_short_tx_series():
    assert _app([248, 600], [505])._check_pipeline_frames() == []


def test_frame_oracle_accepts_startup_only_zero_tx_series():
    assert _app([248, 600], [0])._check_pipeline_frames() == []


def test_frame_oracle_requires_gradeable_rx_series():
    problems = _app([248], [505, 599])._check_pipeline_frames()

    assert any("no gradeable RX stats" in problem for problem in problems)


def test_frame_oracle_diagnostic_names_the_real_wall_clock():
    """The diagnostic must name the window the pipelines really got.

    ``_graded_wall_clock`` and ``_apply_ptp_extension`` both raise the wall
    clock above the requested ``test_time``, so the two numbers routinely
    disagree and an operator triaging this needs the one that was used.
    """
    app = _app([248], [505, 599])
    app.params["test_time"] = 15
    app._wall_clock_s = 30

    problems = app._check_pipeline_frames()

    assert any("ran for 30s" in problem for problem in problems)
    assert not any("ran for 15s" in problem for problem in problems)


def test_frame_oracle_rejects_one_starved_interval():
    problems = _app([248, 600, 100, 600], [505, 599])._check_pipeline_frames()

    assert any("moved 100 RX frames" in problem for problem in problems)


def test_frame_oracle_rejects_starved_tx_interval_when_gradeable():
    problems = _app([248, 600], [505, 100])._check_pipeline_frames()

    assert any("moved 100 TX frames" in problem for problem in problems)


def test_frame_oracle_accepts_exact_p59_floor():
    assert _app([248, 531], [505])._check_pipeline_frames() == []


def test_st20p_frame_oracle_requires_gradeable_rx_series():
    app = GStreamer()
    app.params = {
        "session_type": "st20p",
        "framerate": "p25",
        "test_time": 30,
        "pixel_format": "YUV422PLANAR10LE",
        "width": 1920,
        "height": 1080,
    }
    app._wall_clock_s = 30
    app._results = [
        {"label": "RX", "output": _stats("RX", [100])},
        {"label": "TX", "output": _stats("TX", [100])},
    ]

    problems = app._check_pipeline_frames()

    assert any("no gradeable RX stats" in problem for problem in problems)


def test_audio_byte_sizes_cover_supported_formats():
    assert _AUDIO_SAMPLE_BYTES.keys() == GSTREAMER_AUDIO_FORMAT_MAP.keys()


def test_capture_intent_uses_gstreamer_destination():
    app = GStreamer()
    app.params["destination_ip"] = "239.1.1.1"

    assert app.capture_intent().dst_ips == ("239.1.1.1",)


def test_pipeline_exit_rejects_forced_termination():
    app = GStreamer()

    for return_code in (None, -9, 137):
        problems = app._check_pipeline_exit(
            {"label": "TX", "return_code": return_code, "output": ""}
        )
        assert any("did not exit cleanly" in problem for problem in problems)


def test_pipeline_exit_rejects_element_error():
    app = GStreamer()
    problems = app._check_pipeline_exit(
        {
            "label": "RX",
            "return_code": 0,
            "output": "0:00:01.0 ERROR mtlst20prx failed to create MTL session",
        }
    )

    assert any("element error" in problem for problem in problems)


def test_pipeline_exit_rejects_early_clean_exit():
    app = GStreamer()
    problems = app._check_pipeline_exit(
        {
            "label": "RX",
            "return_code": 0,
            "output": "",
            "exited_before_stop": True,
        }
    )

    assert any("before the test window ended" in problem for problem in problems)


def test_process_group_records_early_exit(monkeypatch):
    class ExitedProcess:
        running = False

    app = GStreamer()
    monkeypatch.setattr(app, "start_process", lambda *_args: ExitedProcess())
    monkeypatch.setattr(app, "_stop_unbounded_proc", lambda *_args: None)
    monkeypatch.setattr(app, "capture_stdout", lambda *_args: "")
    spec = ProcSpec(cmd="unused", host=object(), label="RX", bounded=False)

    app._run_proc_group([spec], build="unused", test_time=0, wall_clock_seconds=0)

    assert spec.exited_before_stop is True


def test_process_group_records_unknown_liveness(monkeypatch):
    class UnknownProcess:
        @property
        def running(self):
            raise RuntimeError("process state unavailable")

    app = GStreamer()
    monkeypatch.setattr(app, "start_process", lambda *_args: UnknownProcess())
    monkeypatch.setattr(app, "_stop_unbounded_proc", lambda *_args: None)
    monkeypatch.setattr(app, "capture_stdout", lambda *_args: "")
    spec = ProcSpec(cmd="unused", host=object(), label="RX", bounded=False)

    app._run_proc_group([spec], build="unused", test_time=0, wall_clock_seconds=0)
    problems = app._check_pipeline_exit(
        {
            "label": spec.label,
            "return_code": 0,
            "output": "",
            "exited_before_stop": spec.exited_before_stop,
        }
    )

    assert spec.exited_before_stop is None
    assert any("could not be verified" in problem for problem in problems)


def test_process_group_snapshots_all_liveness_before_stopping(monkeypatch):
    class RunningProcess:
        running = True

    rx_proc = RunningProcess()
    tx_proc = RunningProcess()
    processes = iter((rx_proc, tx_proc))
    app = GStreamer()
    monkeypatch.setattr(app, "start_process", lambda *_args: next(processes))

    def stop_process(proc, *_args):
        if proc is rx_proc:
            tx_proc.running = False

    monkeypatch.setattr(app, "_stop_unbounded_proc", stop_process)
    monkeypatch.setattr(app, "capture_stdout", lambda *_args: "")
    specs = [
        ProcSpec(cmd="rx", host=object(), label="RX", bounded=False),
        ProcSpec(cmd="tx", host=object(), label="TX", bounded=False),
    ]

    app._run_proc_group(specs, build="unused", test_time=0, wall_clock_seconds=0)

    assert [spec.exited_before_stop for spec in specs] == [False, False]


def _set_ip_pools(monkeypatch):
    monkeypatch.setattr(ip_pools, "tx", ["192.0.2.1"])
    monkeypatch.setattr(ip_pools, "rx", ["192.0.2.2"])
    monkeypatch.setattr(ip_pools, "rx_multicast", ["239.1.1.1"])


def test_st20p_command_translates_v210(monkeypatch):
    _set_ip_pools(monkeypatch)
    app = GStreamer()
    rx_command, _ = app.create_command(
        session_type="st20p",
        nic_port_list=["0000:01:00.0", "0000:01:00.1"],
        pixel_format="v210",
        transport_format="YUV_422_10bit",
        input_file="/mnt/media/input.yuv",
    )

    assert "rawvideoparse format=v210" in app._tx_commands[0]
    assert "dev-port=0000:01:00.0" in app._tx_commands[0]
    assert "rx-pixel-format=v210" in rx_command
    assert "dev-port=0000:01:00.1" in rx_command


def test_st30p_command_rejects_44_1_khz(monkeypatch):
    _set_ip_pools(monkeypatch)
    app = GStreamer()

    try:
        app.create_command(
            session_type="st30p",
            nic_port_list=["0000:01:00.0", "0000:01:00.1"],
            audio_format="PCM16",
            audio_channels=["U02"],
            audio_sampling="44.1kHz",
            input_file="/mnt/media/input.pcm",
        )
    except ValueError as error:
        assert "44.1 kHz" in str(error)
    else:
        raise AssertionError("unsupported 44.1 kHz audio was accepted")


def test_create_command_rejects_unsupported_packing(monkeypatch):
    _set_ip_pools(monkeypatch)
    app = GStreamer()

    try:
        app.create_command(
            session_type="st20p",
            nic_port_list=["0000:01:00.0", "0000:01:00.1"],
            packing="GPM",
            input_file="/mnt/media/input.yuv",
        )
    except ValueError as error:
        assert "packing=GPM" in str(error)
    else:
        raise AssertionError("unsupported packing was silently ignored")


def _st20p_app(monkeypatch, **overrides) -> GStreamer:
    monkeypatch.setattr(ip_pools, "tx", ["192.168.0.1"])
    monkeypatch.setattr(ip_pools, "rx", ["192.168.0.2"])
    app = GStreamer()
    app.params.update(
        session_type="st20p",
        nic_port_list=["0000:00:00.0", "0000:00:00.1"],
        width=1920,
        height=1080,
        framerate="p25",
        pixel_format="YUV422PLANAR10LE",
        input_file="/mnt/media/input.yuv",
        source_ip="192.168.0.1",
        destination_ip="192.168.0.2",
        test_time=30,
    )
    app.params.update(overrides)
    return app


def test_st20p_rx_dump_is_unbounded_like_the_other_adapters(monkeypatch):
    app = _st20p_app(monkeypatch)

    _, rx_cmd = app._build_st20p_cmds(app.params["nic_port_list"])

    # A one-frame sink would make the byte oracle and the MD5 integrity check
    # pass on a session that delivered a single frame out of the whole window.
    assert "filesink location={out}" in rx_cmd
    assert "multifilesink" not in rx_cmd
    assert "max-files" not in rx_cmd


def test_st20p_expected_bytes_cover_the_whole_window(monkeypatch):
    app = _st20p_app(monkeypatch)

    assert app._expected_rx_bytes() == 1920 * 1080 * 4 * 25 * 30


def test_st20p_expected_bytes_scale_with_framerate_and_time(monkeypatch):
    app = _st20p_app(monkeypatch, framerate="p59", test_time=45)

    # p59 truncates to 59, not 59.94, so the floor stays conservative. 45s is
    # past _MIN_GRADED_WALL_CLOCK_S, so it is the window itself -- the clamp
    # below covers the shorter side.
    assert app._expected_rx_bytes() == 1920 * 1080 * 4 * 59 * 45


def test_st20p_expected_bytes_use_mtls_compact_v210_size(monkeypatch):
    app = _st20p_app(
        monkeypatch,
        pixel_format="v210",
        framerate="p25",
        test_time=_MIN_GRADED_WALL_CLOCK_S,
    )

    # These bytes are an MTL RX dump, so the floor has to follow st_frame_size()
    # -- packed 3 pixels per 8 bytes with no row padding -- and not GStreamer's
    # 48-pixel-padded stride. 1920 is a multiple of 48 so the two agree here;
    # test_video_integrity.py covers the width where they do not.
    assert app._expected_rx_bytes() == 1920 * 1080 * 8 // 3 * 25 * (
        _MIN_GRADED_WALL_CLOCK_S
    )


def _st30p_app(**overrides) -> GStreamer:
    app = GStreamer()
    app.params.update(
        session_type="st30p",
        audio_format="PCM16",
        audio_channels=["U02"],
        audio_sampling="48kHz",
        test_time=10,
    )
    app.params.update(overrides)
    return app


# 48kHz x 2ch x 2 bytes.
_ST30P_BYTES_PER_S = 48000 * 2 * 2


def test_expected_bytes_follow_the_extended_window():
    """A short run's floor has to grow with the window ``_graded_wall_clock`` gives it.

    st30p is not in ``_RATE_CHECKED_SESSIONS``, so this floor is its whole
    throughput verdict. Sized on the requested 10s while the run actually streams
    for 30, it asks for 5s of audio out of ~24 delivered -- a session that died
    after 5s would pass.
    """
    app = _st30p_app()

    assert app._expected_rx_bytes() == _ST30P_BYTES_PER_S * _MIN_GRADED_WALL_CLOCK_S


def test_expected_bytes_exclude_ptp_dead_time():
    """PTP sync seconds move no frames, so they must not be charged for.

    This is why the clamp is ``_MIN_GRADED_WALL_CLOCK_S - ptp_dead`` and not a
    plain ``max(test_time, _MIN_GRADED_WALL_CLOCK_S)``: the latter would demand
    30s of audio from a run that streams for 10 and spends 50 waiting for a lock,
    failing a healthy session.
    """
    app = _st30p_app(enable_ptp=True, ptp_sync_time=50)

    assert app._expected_rx_bytes() == _ST30P_BYTES_PER_S * 10


def test_expected_bytes_keep_a_long_window_untouched():
    """No CI leg is affected: past the minimum, the window is test_time verbatim."""
    app = _st30p_app(test_time=60)

    assert app._expected_rx_bytes() == _ST30P_BYTES_PER_S * 60


def _with_rx_dump(app: GStreamer, size: int) -> None:
    """Point ``_check_rx_output`` at an RX dump of exactly ``size`` bytes."""
    app._output_files = ["/mnt/ramdisk/media/out.yuv"]
    app._host = SimpleNamespace(
        connection=SimpleNamespace(
            execute_command=lambda *_args, **_kwargs: SimpleNamespace(
                return_code=0, stdout=str(size)
            )
        )
    )


def test_rx_byte_floor_rejects_a_short_dump(monkeypatch):
    """The floor, not just the existence check, has to fail a truncated dump.

    Everything above pins what ``_expected_rx_bytes`` computes; this is the one
    case that pins the comparison it feeds. Without it, dropping the ``size <
    minimum`` branch leaves the tier green while a session that delivered a
    handful of frames passes on a non-empty file.
    """
    app = _st20p_app(monkeypatch)
    expected = app._expected_rx_bytes()
    _with_rx_dump(app, int(expected * 0.5) - 1)

    problems = app._check_rx_output()

    assert len(problems) == 1
    assert "50% of" in problems[0]


def test_rx_byte_floor_accepts_a_dump_at_the_floor(monkeypatch):
    """Half the nominal bytes is the pass/fail boundary, and it passes.

    The floor is deliberately generous -- pipeline startup, and PTP sync when
    enabled, are unpaid seconds inside the window -- so a run that clears it by
    one byte must not be reported as a delivery failure.
    """
    app = _st20p_app(monkeypatch)
    expected = app._expected_rx_bytes()
    _with_rx_dump(app, int(expected * 0.5) + 1)

    assert app._check_rx_output() == []


def test_graded_window_extends_a_too_short_window():
    for session_type in ("st20p", "st30p", "st40p"):
        app = GStreamer()
        app.params["session_type"] = session_type

        # Every session type gets the floor: st20p/st40p because the frame-rate
        # oracle needs completed stats intervals, st30p because its byte floor
        # is a fraction of framerate x window and a short window is mostly
        # pipeline startup.
        assert app._graded_wall_clock(15) == 30, session_type


def test_graded_window_keeps_a_sufficient_window():
    app = GStreamer()
    app.params["session_type"] = "st20p"

    assert app._graded_wall_clock(30) == 30
    assert app._graded_wall_clock(90) == 90


def test_window_floor_moves_the_wall_clock_not_the_requested_test_time(monkeypatch):
    """The floor must move the wall clock, never ``params["test_time"]`` itself.

    ``capture_intent`` takes a fixed-length sample of the stream and is sized
    from the requested value, so charging it for seconds the caller did not ask
    for would fail a healthy run. ``_expected_rx_bytes`` is the deliberate
    exception -- the RX dump really does grow for the whole extended window -- so
    it follows the floor instead.
    """
    app = _st20p_app(monkeypatch, test_time=15)

    assert app._graded_wall_clock(15) == 30
    assert app.params["test_time"] == 15
    assert app.capture_intent().capture_time == 15
    assert app._expected_rx_bytes() == 1920 * 1080 * 4 * 25 * _MIN_GRADED_WALL_CLOCK_S


def test_execute_test_runs_for_the_graded_window(monkeypatch):
    """The floor is worthless unless ``execute_test`` actually waits for it.

    ``_graded_wall_clock`` returning 30 changes nothing if the run still stops
    at the configured 15s: the frame oracle then grades a window that cannot
    hold :data:`_MIN_GRADED_INTERVALS` completed stats intervals and fails a
    healthy session. This pins the wiring, not the arithmetic.
    """
    app = _st20p_app(monkeypatch, test_time=15)
    app.command = "gst-launch-1.0 rx"
    app._tx_commands = ["gst-launch-1.0 tx"]

    seen: dict = {}
    monkeypatch.setattr(app, "prepare_execution", lambda **_kwargs: None)
    monkeypatch.setattr(app, "capture_intent", lambda: None)
    monkeypatch.setattr(app, "integrity_intent", lambda *_args: None)
    monkeypatch.setattr(app, "_finalize_run", lambda *_args, **_kwargs: True)
    monkeypatch.setattr(
        app, "_run_proc_group", lambda _specs, **kwargs: seen.update(kwargs)
    )

    assert app.execute_test(build="/unused", test_time=15, host=object()) is True

    # Both the sleep and the value validate_results reports against must move.
    assert seen["wall_clock_seconds"] == 30
    assert seen["test_time"] == 30
    assert app._wall_clock_s == 30
    # ...while the requested window, which sizes the byte oracle, must not.
    assert app.params["test_time"] == 15


def test_ffmpeg_create_command_rejects_unsupported_session():
    with pytest.raises(ValueError, match="st40p"):
        FFmpeg(app_path="/unused").create_command(session_type="st40p")


@pytest.mark.parametrize(
    "pixel_format",
    ["YUV422PLANAR8", "YUV420PLANAR8", "YUV420PLANAR10LE", "RGBA"],
)
def test_ffmpeg_reports_formats_not_represented_by_plugin(pixel_format):
    reason = FFmpeg(app_path="/unused").unsupported_reason(
        session_type="st20p", pixel_format=pixel_format
    )

    assert reason is not None
    assert pixel_format in reason


def test_gstreamer_reports_unsupported_session():
    reason = GStreamer().unsupported_reason(session_type="st22p")

    assert reason is not None
    assert "st22p" in reason
