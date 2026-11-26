# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""Offline tests of the stability engine: no NIC, no root, no EBU LIST.

cd tests/acceptance && python -m pytest --noconftest -q stability/tests
"""

import json

import pytest

from stability import config, ebu, logscan, workload


def _host(**kw) -> config.HostConfig:
    h = config.HostConfig(
        mtl_path="/mtl",
        rxtxapp="/mtl/build/RxTxApp",
        ffmpeg="ffmpeg",
        gst_launch="gst-launch-1.0",
        st22_codec="jpegxs",
        tx_ports=["0000:01:00.0", "0000:04:00.0", "0000:05:00.0"],
        rx_ports=["0000:06:00.0", "0000:07:00.0", "0000:08:00.0"],
        plugins={
            "st22_svt_jpegxs": "/p/jxs.so",
            "color_convert_sample": "/p/conv.so",
        },
        media_dir="/dev/shm/m",
        **kw,
    )
    h.cpus = config.CpuConfig(housekeeping="0-1", app="2-7", lcores="8-15")
    h.lcore_slices = config.default_lcore_slices(config.cpu_list(h.cpus.lcores))
    return h


def test_cpu_list_round_trip():
    assert config.cpu_list("2-4,8, 10-11") == [2, 3, 4, 8, 10, 11]
    assert config.cpu_text([8, 2, 3, 4, 10, 11]) == "2-4,8,10-11"
    assert config.cpu_text([]) == ""


def test_lcore_slices_do_not_overlap_and_fit():
    slices = config.default_lcore_slices(list(range(8, 16)))
    used = [c for s in slices.values() for c in config.cpu_list(s)]
    assert len(used) == len(set(used))
    assert set(used) <= set(range(8, 16))
    assert len(config.cpu_list(slices["rxtxapp-mix"])) >= 2


def test_full_plan_has_each_api_and_unique_groups():
    plan = workload.build_plan(_host(), "full", 600)
    apis = {s.api for s in plan.streams}
    assert {"st20p", "st20", "st22p", "st30p", "st30", "st40p", "st40", "st41"} <= apis
    dsts = [(s.dip, s.port) for s in plan.streams]
    assert len(dsts) == len(set(dsts))
    assert len({s.dip for s in plan.streams}) == len(plan.streams)
    assert {w.kind for w in plan.workers} == {"rxtxapp", "ffmpeg", "gstreamer"}
    assert len({s.sid for s in plan.streams}) == len(plan.streams)


def test_each_mix_session_has_a_receiver_and_user_pacing_where_planned():
    plan = workload.build_plan(_host(), "full", 600)
    mix = json.loads(
        next(w for w in plan.workers if w.name == "rxtxapp-mix").files["mix.json"]
    )
    rx = json.loads(
        next(w for w in plan.workers if w.name == "rxtxapp-rx").files["rx.json"]
    )
    tx_groups = sorted(g["dip"][0] for g in mix["tx_sessions"])
    rx_groups = sorted(g["ip"][0] for g in rx["rx_sessions"])
    assert set(tx_groups) <= set(
        rx_groups
    )  # rx also takes the streams no other worker receives
    st20p = [g["st20p"][0] for g in mix["tx_sessions"] if "st20p" in g]
    assert all(s["user_pacing"] for s in st20p)
    # an exact frame takes its RTP time from the epoch, the others from the user time
    assert all(
        bool(s.get("exact_user_pacing")) != bool(s.get("user_timestamp")) for s in st20p
    )
    assert any(s.get("exact_user_pacing") for s in st20p)
    mix_w = next(w for w in plan.workers if w.name == "rxtxapp-mix")
    assert "--timestamp_epoch" in mix_w.argv
    inputs = {s["input_format"] for s in st20p}
    assert {"YUV422PLANAR10LE", "V210", "YUV422RFC4175PG2BE10"} <= inputs
    assert {s["fps"] for s in st20p} == {"p59", "p50"}


def test_extconv_worker_loads_only_the_converter_plugin():
    plan = workload.build_plan(_host(), "full", 600)
    ext = next(w for w in plan.workers if w.name == "rxtxapp-extconv")
    names = [p["name"] for p in json.loads(ext.files["kahawai.json"])["plugins"]]
    assert names == ["color_convert_sample"]
    mix = next(w for w in plan.workers if w.name == "rxtxapp-mix")
    names = [p["name"] for p in json.loads(mix.files["kahawai.json"])["plugins"]]
    assert names == ["st22_svt_jpegxs"]


def test_smoke_is_rxtxapp_only_and_plan_json_round_trips():
    plan = workload.build_plan(_host(), "smoke", 60)
    assert {w.kind for w in plan.workers} == {"rxtxapp"}
    again = workload.Plan.from_json(plan.to_json())
    assert again.streams == plan.streams
    assert [w.name for w in again.workers] == [w.name for w in plan.workers]


def test_rxtxapp_argv_carries_lcores_and_pacing():
    h = _host(pacing_way="tsc")
    plan = workload.build_plan(h, "smoke", 60)
    mix = next(w for w in plan.workers if w.name == "rxtxapp-mix")
    assert mix.argv[mix.argv.index("--lcores") + 1] == h.lcore_slices["rxtxapp-mix"]
    assert mix.argv[mix.argv.index("--pacing_way") + 1] == "tsc"
    assert mix.argv[mix.argv.index("--test_time") + 1] == "60"


def test_packets_of_a_capture():
    s = workload.Stream(
        "v",
        "video",
        "st20p",
        "w",
        "239.0.0.1",
        20000,
        width=1920,
        height=1080,
        fps="p59",
        transport_format="YUV_422_10bit",
    )
    # 1080p 4:2:2 10 bit: 5184000 bytes per frame, 1200 per packet -> 4321 packets per frame
    assert s.packets_per_second() == pytest.approx(4321 * 60000 / 1001)
    a = workload.Stream(
        "a", "audio", "st30p", "w", "239.0.0.2", 20002, audio_ptime="0.125"
    )
    assert a.packets_per_second() == pytest.approx(8000)
    assert a.capture_packets(0.001) == 64


# --- logscan ---------------------------------------------------------------


def _scanner(tmp_path, text):
    log = tmp_path / "w.log"
    log.write_text(text)
    s = logscan.LogScanner(["w"], lambda _w: str(log))
    s.scan()
    return s, log


def test_fps_lines_and_results(tmp_path):
    s, _ = _scanner(
        tmp_path,
        "MTL: TX_VIDEO_SESSION(0,1:st20p_0): fps 59.940060 frames 180 pkts 777780\n"
        "MTL: RX_AUDIO_SESSION(1,0:st30p_0): fps 1000.000 frames 3000 pkts 3000\n"
        "app_rx_st20p_result(0), OK, fps 59.94, 180 frame received\n"
        "app_rx_st30p_result(1), FAILED, fps 0.0\n",
    )
    sm = s.summary()
    assert sm["sessions"]["w/TX_VIDEO_SESSION(0,1:st20p_0)"][
        "median_fps"
    ] == pytest.approx(59.94, abs=1e-3)
    assert "w/RX_AUDIO_SESSION(1,0:st30p_0)" in sm["sessions"]
    assert sm["results"] == {
        "w/app_rx_st20p_result(0)": "OK",
        "w/app_rx_st30p_result(1)": "FAILED",
    }
    assert sm["error_counts"] == {"w/result-failed": 1}


def test_scan_reads_only_new_whole_lines(tmp_path):
    s, log = _scanner(tmp_path, "Segmentation fault (core dumped)\npartial li")
    assert s.counts[("w", "crash")] == 1
    with open(log, "a") as f:
        f.write("ne\nError: something broke\n")
    s.scan()
    assert s.counts[("w", "crash")] == 1
    assert s.counts[("w", "mtl-error")] == 1


def test_benign_lines_are_not_errors(tmp_path):
    s, _ = _scanner(
        tmp_path,
        "MTL: dev_if_select_pacing(0), not support tm rl, try tsc\n"
        "app_tx_st20p_result(0), OK, fps 59.94\n",
    )
    assert not s.counts


def test_a_stall_after_warmup_is_an_anomaly(tmp_path, monkeypatch):
    log = tmp_path / "w.log"
    log.write_text("")
    s = logscan.LogScanner(["w"], lambda _w: str(log))
    s.start -= 100  # past the warm-up
    for fps in (59.94, 59.94, 59.94, 59.94, 30.0):
        with open(log, "a") as f:
            f.write(f"TX_VIDEO_SESSION(0,0:x): fps {fps} frames 1\n")
        s.scan()
    assert len(s.anomalies) == 1
    assert "fps 30.000" in s.anomalies[0][2]


def test_a_bad_counter_above_zero_is_kept(tmp_path):
    s, _ = _scanner(
        tmp_path,
        "RX_VIDEO_SESSION(0,0): incomplete frames 3\nRX_VIDEO_SESSION(0,0): incomplete frames 0\n",
    )
    assert s.summary()["bad_counters"] == {
        "w/RX_VIDEO_SESSION(0,0)/incomplete frames": 3
    }


# --- EBU LIST verdict -----------------------------------------------------------

VIDEO = workload.Stream(
    "v",
    "video",
    "st20p",
    "w",
    "239.30.1.1",
    20002,
    width=1920,
    height=1080,
    fps="p59",
    packing="BPM",
    transport_format="YUV_422_10bit",
)


def _report(compliance="narrow", **media):
    ms = {
        "width": 1920,
        "height": 1080,
        "rate": "60000/1001",
        "packing_mode": 2,
        **media,
    }
    return {
        "analyzed": True,
        "not_compliant_streams": 0,
        "streams": [
            {
                "media_type": "video",
                "state": "analyzed",
                "network_information": {"destination_address": "239.30.1.1"},
                "media_specific": ms,
                "global_video_analysis": {"compliance": compliance},
                "analyses": {"rtp_ts_vs_nt": {"result": "compliant"}},
            }
        ],
    }


def test_judge_pass_wide_and_fail():
    assert ebu.judge(VIDEO, _report(), allow_wide=False)[0] == "pass"
    assert ebu.judge(VIDEO, _report("wide"), allow_wide=True)[0] == "pass-wide"
    verdict, reasons, _ = ebu.judge(VIDEO, _report("wide"), allow_wide=False)
    assert verdict == "fail" and "wide" in reasons[0]
    verdict, reasons, _ = ebu.judge(VIDEO, _report(width=1280), allow_wide=False)
    assert verdict == "fail" and any("resolution" in r for r in reasons)


def test_judge_names_each_failed_analysis():
    r = _report()
    r["streams"][0]["analyses"]["packet_ts_vs_rtp_ts"] = {"result": "not_compliant"}
    r["not_compliant_streams"] = 1
    verdict, reasons, _ = ebu.judge(VIDEO, r, allow_wide=False)
    assert verdict == "fail"
    assert reasons == ["packet_ts_vs_rtp_ts=not_compliant"]


def test_judge_no_stream():
    assert (
        ebu.judge(VIDEO, {"analyzed": True, "streams": []}, allow_wide=False)[0]
        == "no-stream"
    )


def test_audio_description_for_ebu_list():
    s = workload.Stream(
        "a",
        "audio",
        "st30p",
        "w",
        "239.0.0.2",
        2,
        audio_format="PCM24",
        audio_sampling="96kHz",
        audio_ptime="0.125",
        channels=2,
    )
    assert ebu.media_specific_audio(s) == {
        "encoding": "L24",
        "sampling": "96000",
        "number_channels": 2,
        "packet_time": "0.125000",
    }


def test_a_port_by_mac_comes_from_the_table_when_the_vf_has_no_netdev(tmp_path):
    table = tmp_path / "vf-macs"
    table.write_text("02:00:15:00:00:05 0000:03:00.0\n")
    assert config.resolve_port("02:00:15:00:00:05", str(table)) == "0000:03:00.0"
    assert config.resolve_port("0000:09:00.0", str(table)) == "0000:09:00.0"
    with pytest.raises(ValueError):
        config.resolve_port("02:00:15:00:00:99", str(table))


def test_password_from_either_key(tmp_path):
    f = tmp_path / "ebu.env"
    f.write_text("EBU_LIST_USERNAME=stability\nEBU_LIST_PASSWORD=pw1\n")
    assert config.EbuConfig(password_file=str(f)).password() == "pw1"
    f.write_text("EBU_PASSWORD=pw2\n")
    assert config.EbuConfig(password_file=str(f)).password() == "pw2"


def test_user_time_resyncs_are_counted(tmp_path):
    s, _ = _scanner(
        tmp_path,
        "app_tx_st20p_uinit(0), user time resync 1\napp_tx_st30p_uinit(2), user time resync 7\n",
    )
    assert s.summary()["resyncs"] == {"w/app_tx_st20p(0)": 1, "w/app_tx_st30p(2)": 7}
    assert not s.counts


def test_the_integrity_worker_takes_the_4th_ports_and_only_it_checks_sha():
    h = _host()
    h.tx_ports.append("0000:09:00.0")
    h.rx_ports.append("0000:0a:00.0")
    plan = workload.build_plan(h, "smoke", 60)
    names = [w.name for w in plan.workers]
    assert "rxtxapp-integrity" in names
    for w in plan.workers:
        assert ("--video_sha_check" in w.argv) == (w.name == "rxtxapp-integrity")
    integ = next(w for w in plan.workers if w.name == "rxtxapp-integrity")
    assert integ.ports == ["0000:09:00.0", "0000:0a:00.0"]
    cfg = json.loads(integ.files["integrity.json"])
    assert len(cfg["tx_sessions"]) == len(cfg["rx_sessions"]) == 3
    assert all(not s.ebu for s in plan.streams if s.worker == "rxtxapp-integrity")


def test_yaml_ints_become_cpu_lists(tmp_path):
    cfg = tmp_path / "s.yaml"
    cfg.write_text(
        "mtl_path: /mtl\nports: {tx: ['0000:01:00.0'], rx: ['0000:02:00.0']}\n"
        "cpus: {housekeeping: 0, app: 1-3, lcores: 4-7}\nlcore_slices: {rxtxapp-extconv: 20}\n"
    )
    h = config.load(str(cfg))
    assert h.lcore_slices == {"rxtxapp-extconv": "20"}
    assert h.cpus.housekeeping == "0"


def test_not_judged_analyses_and_allow_wide_are_per_stream():
    s = workload.Stream(
        **{**VIDEO.__dict__, "not_judged": ["packet_ts_vs_rtp_ts", "2110_21_vrx"]}
    )
    r = _report("not_compliant")
    r["streams"][0]["analyses"]["packet_ts_vs_rtp_ts"] = {"result": "not_compliant"}
    r["streams"][0]["analyses"]["2110_21_vrx"] = {"result": "not_compliant"}
    r["not_compliant_streams"] = 1
    verdict, reasons, metrics = ebu.judge(s, r, allow_wide=False)
    assert verdict == "pass", reasons
    assert sorted(metrics["not_judged"]) == [
        "2110_21_vrx=not_compliant",
        "packet_ts_vs_rtp_ts=not_compliant",
    ]
    w = workload.Stream(**{**VIDEO.__dict__, "allow_wide": True})
    assert ebu.judge(w, _report("wide"), allow_wide=False)[0] == "pass-wide"


def test_jxsv_sdp_has_the_mandatory_lines():
    s = workload.Stream(
        "x",
        "video",
        "st22p",
        "w",
        "239.30.1.10",
        20020,
        width=1920,
        height=1080,
        fps="p59",
        compressed=True,
        sip="192.168.30.11",
    )
    sdp = ebu.sdp_jxsv(s)
    for want in (
        "m=video 20020 RTP/AVP 114",
        "c=IN IP4 239.30.1.10/64",
        "a=rtpmap:114 jxsv/90000",
        "width=1920",
        "height=1080",
        "TP=2110TPNL",
        "b=AS:",
        "a=framerate:59.94",
        "exactframerate=60000/1001",
    ):
        assert want in sdp


def test_no_two_workers_share_a_port():
    h = _host()
    h.tx_ports.append("0000:09:00.0")
    h.rx_ports.append("0000:0a:00.0")
    plan = workload.build_plan(h, "full", 600)
    seen = {}
    for w in plan.workers:
        for p in w.ports:
            assert seen.setdefault(p, w.name) == w.name, (p, seen[p], w.name)
    assert {"rxtxapp-integrity", "ffmpeg-rx", "gst-tx"} <= {
        w.name for w in plan.workers
    }


def _pcap(path, pkts):
    """A nanosecond pcap of RFC 4175 packets: (t ns, seq32, rtp ts, marker)."""
    import struct as st

    with open(path, "wb") as f:
        f.write(st.pack("<IHHiIII", 0xA1B23C4D, 2, 4, 0, 0, 65535, 1))
        for t, seq, ts, m in pkts:
            eth = b"\x01\x00\x5e\x1e\x01\x02" + b"\x02" * 6 + b"\x08\x00"
            ip = bytes([0x45]) + b"\x00" * 19
            udp = b"\x00" * 8
            rtp = bytes([0x80, (0x80 if m else 0) | 112]) + st.pack(
                ">HII", seq & 0xFFFF, ts, 0x1234
            )
            pay = st.pack(">H", seq >> 16) + b"\x00" * 1260
            d = eth + ip + udp + rtp + pay
            f.write(st.pack("<IIII", t // 10**9, t % 10**9, len(d), len(d)) + d)


def test_pcaptool_reports_grid_offset_breaks_and_rtp(tmp_path):
    from stability import pcaptool

    T = 10**9 * 1001 // 60000  # ~16683333 ns
    epoch0 = 107629997467 * 10**9 * 1001 // 60000
    pkts, seq = [], 70000
    for fr in range(4):
        start = epoch0 + fr * T + 620_000  # 620 us after each epoch
        rtp = (epoch0 + fr * T) * 90000 // 10**9 % 2**32
        for i in range(10):
            if fr == 2 and i == 5:
                seq += 3  # three packets missing
            pkts.append((start + i * 4000, seq, rtp, i == 9))
            seq += 1
    p = tmp_path / "s.pcap"
    _pcap(p, pkts)
    r = pcaptool.analyze(str(p), "p59")
    assert r["frames"] == 3 and r["seq_breaks"] == 1
    assert r["first_breaks"][0]["missing"] == 3
    assert abs(r["first_minus_epoch_us"]["median"] - 620) < 1
    assert abs(r["pkt_minus_rtp_us"]["median"] - 620) < 15  # 90 kHz rounding


def test_each_stream_of_the_full_plan_has_a_receiver():
    h = _host()
    h.tx_ports.append("0000:09:00.0")
    h.rx_ports.append("0000:0a:00.0")
    plan = workload.build_plan(h, "full", 600)
    received = {sid for w in plan.workers for sid in w.receives}
    assert {s.sid for s in plan.streams} <= received
    rx = json.loads(
        next(w for w in plan.workers if w.name == "rxtxapp-rx").files["rx.json"]
    )
    groups = {g["ip"][0] for g in rx["rx_sessions"]}
    for sid in ("gst-st20p-1080p59", "gst-st30p-pcm24-48k", "ffmpeg-st22p-1080p59"):
        assert plan.stream(sid).dip in groups


def test_audio_and_anc_move_to_the_aux_worker_with_a_5th_tx_port():
    h = _host()
    h.tx_ports += ["0000:09:00.0", "0000:0b:00.0"]
    h.rx_ports.append("0000:0a:00.0")
    plan = workload.build_plan(h, "full", 600)
    aux = next(w for w in plan.workers if w.name == "rxtxapp-aux")
    assert aux.ports == ["0000:0b:00.0"]
    moved = {s.sid for s in plan.streams if s.worker == "rxtxapp-aux"}
    assert moved == {
        s.sid
        for s in plan.streams
        if s.media in ("audio", "anc", "fmd")
        and s.worker in ("rxtxapp-aux", "rxtxapp-mix")
    }
    assert moved and set(aux.sends) == moved
    mix = json.loads(
        next(w for w in plan.workers if w.name == "rxtxapp-mix").files["mix.json"]
    )
    kinds = {k for g in mix["tx_sessions"] for k in g if k not in ("dip", "interface")}
    assert not kinds & {"st30p", "audio", "st40p", "ancillary"}
    rx = next(w for w in plan.workers if w.name == "rxtxapp-rx")
    assert moved <= set(rx.receives)
    # with 4 TX ports nothing moves
    h4 = _host()
    h4.tx_ports.append("0000:09:00.0")
    h4.rx_ports.append("0000:0a:00.0")
    assert "rxtxapp-aux" not in {
        w.name for w in workload.build_plan(h4, "full", 600).workers
    }
