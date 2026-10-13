# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Review tests for PR 1796: FFmpeg st20p multi-session and st22p recordings.

Each test runs the real FFmpeg adapter code (create_command,
prepare_execution, execute_test, _finalize_run, validate_results, the real
IntegritySession and the real video_integrity.py) against a fake host that
runs commands on the local file system. Only _run_proc_group is replaced: it
writes the bytes an RX FFmpeg would record, instead of starting a process.
"""

import os
import random
import shlex

import pytest
from mtl_engine.ffmpeg import FFmpeg
from mtl_engine.integrity_session import IntegritySession

W, H = 64, 36
FRAME = W * H * 4  # yuv422p10le
SRC_FRAMES = 10


def _make_source(path):
    rnd = random.Random(1796)
    frames = [
        bytes(rnd.getrandbits(8) for _ in range(FRAME)) for _ in range(SRC_FRAMES)
    ]
    with open(path, "wb") as f:
        f.write(b"".join(frames))
    return frames


def _recording(frames, n, start=0):
    return b"".join(frames[(start + i) % len(frames)] for i in range(n))


def _out_paths(cmd):
    """Output paths in a built RX command: the token after each -f rawvideo [-fs N]."""
    toks = shlex.split(cmd)
    outs = []
    for i, t in enumerate(toks):
        if t == "rawvideo" and toks[i - 1] == "-f":
            j = i + 1
            if toks[j] == "-fs":
                j += 2
            outs.append(toks[j])
    return outs


def _fs_values(cmd):
    toks = shlex.split(cmd)
    return [int(toks[i + 1]) for i, t in enumerate(toks) if t == "-fs"]


def _patch_run(monkeypatch, app, contents):
    """contents: {output path: bytes} -- what the 'RX' writes during the run."""

    def fake_run(specs, **kw):
        for path, data in contents.items():
            with open(path, "wb") as f:
                f.write(data)
        for s in specs:
            s.captured_output = ""
            s.proc = None
        app._host = specs[0].host  # as the real _run_proc_group does
        return specs

    monkeypatch.setattr(app, "_run_proc_group", fake_run)
    monkeypatch.setattr(app, "_safe_return_code", lambda proc: 0)


def _multisession_app(src, out, test_time, cap):
    app = FFmpeg(app_path="/nonexistent")
    app.create_command(
        session_type="st20p",
        nic_port_list=["0000:00:00.1", "0000:00:00.2"],
        width=W,
        height=H,
        framerate="p25",
        pixel_format="YUV422PLANAR10LE",
        input_file=src,
        output_file=out,
        output_format="yuv",
        multiple_sessions=True,
        test_time=test_time,
        rx_max_file_size=cap,
    )
    return app


# ------------------------------------------------------------- command line
def test_multisession_cmd_caps_both_outputs_and_names_them(tmp_path):
    src = str(tmp_path / "src.yuv")
    out = str(tmp_path / "src.yuv.out")
    app = _multisession_app(src, out, 4, 30 * FRAME)
    assert app.rx_output_files() == [out + "_0", out + "_1"]
    # placeholders still in the command before prepare_execution
    assert "{out0}" in app.command and "{out1}" in app.command
    assert _fs_values(app.command) == [30 * FRAME, 30 * FRAME]


def test_multisession_prepare_resolves_distinct_files(tmp_path, host, repo):
    src = str(tmp_path / "src.yuv")
    _make_source(src)
    out = str(tmp_path / "src.yuv.out")
    app = _multisession_app(src, out, 4, 30 * FRAME)
    app.params["test_time"] = 4
    app.prepare_execution(build=repo, host=host)
    outs = _out_paths(app.command)
    assert outs == [out + "_0", out + "_1"]
    # -map 0:0 goes to _0 and -map 1:0 to _1
    toks = shlex.split(app.command)
    assert toks[toks.index(outs[0]) - 6 : toks.index(outs[0])][:2] == ["-map", "0:0"]
    assert toks[toks.index(outs[1]) - 6 : toks.index(outs[1])][:2] == ["-map", "1:0"]
    assert all(os.path.exists(p) for p in outs)


# ------------------------------------------------- validation, both streams
@pytest.mark.parametrize(
    "n0, n1, expect",
    [
        (60, 60, True),  # both full
        (60, 0, False),  # stream 1 empty
        (0, 60, False),  # stream 0 empty
        (60, 10, False),  # stream 1 short
        (60, None, False),  # stream 1 missing
    ],
)
def test_multisession_validate_needs_both(
    tmp_path, host, repo, monkeypatch, n0, n1, expect
):
    src = str(tmp_path / "src.yuv")
    frames = _make_source(src)
    out = str(tmp_path / "src.yuv.out")
    test_time = 4  # min_expected_frames(25, 4) = 50
    app = _multisession_app(src, out, test_time, 0)
    contents = {out + "_0": _recording(frames, n0)}
    if n1 is not None:
        contents[out + "_1"] = _recording(frames, n1)
    _patch_run(monkeypatch, app, contents)
    if n1 is None:
        # touch() created it; RX "deletes" it so it is missing at check time
        orig = app.prepare_execution

        def prep(**kw):
            orig(**kw)
            os.remove(out + "_1")

        monkeypatch.setattr(app, "prepare_execution", prep)
    ok = app.execute_test(
        build=repo, test_time=test_time, host=host, fail_on_error=False
    )
    assert ok is expect
    # cleanup removed every recording
    assert not os.path.exists(out + "_0") and not os.path.exists(out + "_1")


@pytest.mark.parametrize("bad_stream", [0, 1])
def test_multisession_integrity_catches_a_bad_frame_in_either_stream(
    tmp_path, host, repo, monkeypatch, bad_stream
):
    src = str(tmp_path / "src.yuv")
    frames = _make_source(src)
    out = str(tmp_path / "src.yuv.out")
    test_time = 4
    app = _multisession_app(src, out, test_time, 0)
    good = _recording(frames, 60, start=3)
    bad = bytearray(_recording(frames, 60, start=5))
    bad[FRAME * 20 + 7] ^= 0xFF
    contents = {out + "_0": good, out + "_1": good}
    contents[f"{out}_{bad_stream}"] = bytes(bad)
    _patch_run(monkeypatch, app, contents)
    session = IntegritySession()
    ok = app.execute_test(
        build=repo,
        test_time=test_time,
        host=host,
        integrity=session,
        fail_on_error=False,
    )
    assert ok is False
    ran = [c for c in host.connection.commands if "video_integrity.py" in c]
    assert len(ran) == 2, ran
    assert "src.yuv.out_0 " in ran[0] and "src.yuv.out_1 " in ran[1]


def test_multisession_integrity_passes_two_good_streams(
    tmp_path, host, repo, monkeypatch
):
    src = str(tmp_path / "src.yuv")
    frames = _make_source(src)
    out = str(tmp_path / "src.yuv.out")
    test_time = 4
    app = _multisession_app(src, out, test_time, 0)
    _patch_run(
        monkeypatch,
        app,
        {out + "_0": _recording(frames, 60, 2), out + "_1": _recording(frames, 55, 7)},
    )
    ok = app.execute_test(
        build=repo,
        test_time=test_time,
        host=host,
        integrity=IntegritySession(),
        fail_on_error=False,
    )
    assert ok is True


def test_multisession_capped_recording_passes(tmp_path, host, repo, monkeypatch):
    """A recording that stopped at the -fs cap (FFmpeg writes whole frames and
    stops on the first one at or past the cap) must pass both checks."""
    src = str(tmp_path / "src.yuv")
    frames = _make_source(src)
    out = str(tmp_path / "src.yuv.out")
    test_time = 40  # 500 frames expected without a cap
    cap = 12 * FRAME + 100  # not a frame multiple
    app = _multisession_app(src, out, test_time, cap)
    n = -(-cap // FRAME)  # ceil: FFmpeg checks the size before each write
    _patch_run(
        monkeypatch,
        app,
        {out + "_0": _recording(frames, n), out + "_1": _recording(frames, n, 4)},
    )
    ok = app.execute_test(
        build=repo,
        test_time=test_time,
        host=host,
        integrity=IntegritySession(),
        fail_on_error=False,
    )
    assert ok is True


# ------------------------------------------------------------------ st22p
def _st22p_app(src, out, test_time, cap, framerate="p25"):
    app = FFmpeg(app_path="/nonexistent")
    app.create_command(
        session_type="st22p",
        test_mode="multicast",
        nic_port_list=["0000:00:00.1", "0000:00:00.2"],
        width=W,
        height=H,
        framerate=framerate,
        codec="JPEG-XS",
        quality="speed",
        pixel_format="YUV422PLANAR10LE",
        input_file=src,
        output_file=out,
        codec_threads=2,
        test_time=test_time,
        rx_max_file_size=cap,
    )
    return app


def test_st22p_cmd_has_cap_before_output(tmp_path):
    out = str(tmp_path / "src.yuv.out")
    app = _st22p_app(str(tmp_path / "src.yuv"), out, 90, 8 * 1024**3)
    toks = shlex.split(app.command)
    i = toks.index("{out0}")
    assert toks[i - 4 : i] == ["-f", "rawvideo", "-fs", str(8 * 1024**3)]


@pytest.mark.parametrize(
    "n, cap, expect",
    [
        (60, 0, True),
        (10, 0, False),
        (0, 0, False),
        (13, 12 * FRAME + 1, True),  # capped
    ],
)
def test_st22p_counts_frames(tmp_path, host, repo, monkeypatch, n, cap, expect):
    src = str(tmp_path / "src.yuv")
    frames = _make_source(src)
    out = str(tmp_path / "src.yuv.out")
    app = _st22p_app(src, out, 4, cap)
    monkeypatch.setattr(app, "with_local_kahawai", lambda cmd, b, c: cmd)
    _patch_run(monkeypatch, app, {out: _recording(frames, n)})
    ok = app.execute_test(build=repo, test_time=4, host=host, fail_on_error=False)
    assert ok is expect
    assert not os.path.exists(out)


# ------------------------------------------- h264 multi-session (finding)
def test_multisession_h264_validate_checks_only_stream_0(
    tmp_path, host, repo, monkeypatch
):
    """With output_format="h264" and multiple_sessions=True the RX records two
    files, but validate_results() hands only _output_files[0] to
    check_output_video_h264(). An empty stream 1 then passes."""
    from mtl_engine import ffmpeg_app

    checked = []

    def fake_h264_check(output_file, video_size, host, build, input_file):
        checked.append(output_file)
        return os.path.getsize(output_file) > 0  # stands in for ffprobe

    monkeypatch.setattr(ffmpeg_app, "check_output_video_h264", fake_h264_check)
    src = str(tmp_path / "src.yuv")
    _make_source(src)
    out = str(tmp_path / "src.yuv.out")
    app = FFmpeg(app_path="/nonexistent")
    app.create_command(
        session_type="st20p",
        nic_port_list=["0000:00:00.1", "0000:00:00.2"],
        width=W,
        height=H,
        framerate="p25",
        pixel_format="YUV422PLANAR10LE",
        input_file=src,
        output_file=out,
        output_format="h264",
        multiple_sessions=True,
        test_time=4,
    )
    _patch_run(monkeypatch, app, {out + "_0": b"\x00\x00\x00\x01h264", out + "_1": b""})
    ok = app.execute_test(build=repo, test_time=4, host=host, fail_on_error=False)
    assert ok is False, "an empty second h264 recording passed"
    assert checked == [out + "_0", out + "_1"], f"checked only {checked}"


def test_built_rx_commands_are_valid_shell(tmp_path, host, repo, monkeypatch):
    import subprocess

    src = str(tmp_path / "src.yuv")
    _make_source(src)
    ms = _multisession_app(src, str(tmp_path / "a.out"), 4, 4 * 1024**3)
    ms.params["test_time"] = 4
    ms.prepare_execution(build=repo, host=host)
    st = _st22p_app(src, str(tmp_path / "b.out"), 4, 8 * 1024**3)
    monkeypatch.setattr(st, "with_local_kahawai", lambda cmd, b, c: cmd)
    st.params["test_time"] = 4
    st.prepare_execution(build=repo, host=host)
    for cmd in [ms.command, *ms._tx_commands, st.command, *st._tx_commands]:
        assert "{" not in cmd, cmd
        r = subprocess.run(["bash", "-n", "-c", cmd], capture_output=True, text=True)
        assert r.returncode == 0, (cmd, r.stderr)
