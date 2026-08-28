# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from mtl_engine import ip_pools
from mtl_engine.application_base import ProcSpec
from mtl_engine.config.mappings import GSTREAMER_AUDIO_FORMAT_MAP
from mtl_engine.ffmpeg import FFmpeg
from mtl_engine.gstreamer import _AUDIO_SAMPLE_BYTES, GStreamer


def _stats(direction: str, frames: list[int]) -> str:
    return "\n".join(
        f"{direction}_st40p(0), frame get try {count} succ {count}, put {count}"
        for count in frames
    )


def _app(rx: list[int], tx: list[int]) -> GStreamer:
    app = GStreamer()
    app.params = {"session_type": "st40p", "framerate": "p59", "test_time": 30}
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


def test_st20p_command_bounds_rx_output_to_one_frame(monkeypatch):
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
        input_file="/tmp/input.yuv",
        source_ip="192.168.0.1",
        destination_ip="192.168.0.2",
    )

    _, rx_cmd = app._build_st20p_cmds(app.params["nic_port_list"])

    assert "multifilesink location={out} max-files=1" in rx_cmd
    assert app._expected_rx_bytes() == 1920 * 1080 * 4


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
