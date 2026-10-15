# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import random
import subprocess
from types import SimpleNamespace

import pytest
from mtl_engine.integrity import st40p_pass, st40p_recording_errors

DIT, K_BIT, MIN_FRAMES = 0x3FFFFF, 1, 100
# As fmd_payload in st40p/test_fast_metadata.py: 64 KiB of non-zero bytes and
# "end", so that the last data item of a pass is padded.
SRC = bytes(b % 255 + 1 for b in random.Random(0).randbytes(65536)) + b"end"
PASS = SRC + b"\0"
ITEMS = [PASS[i : i + 1024] for i in range(0, len(PASS), 1024)]
FMD_LINE = (
    "meta=0 rtp_marker=0 seq_discont=0 seq_lost=0 pkts_total=1 pkts_recv_p=1 "
    f"pkts_recv_r=0 fmd_dit={DIT} fmd_k_bit={K_BIT}"
)
ANC_LINE = (
    "meta=1 rtp_marker=1 seq_discont=0 seq_lost=0 pkts_total=1 pkts_recv_p=1 "
    "pkts_recv_r=0"
)


class _Connection:
    """Runs each host command here, as the remote shell would."""

    def execute_command(self, cmd, **_):
        res = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        return SimpleNamespace(return_code=res.returncode, stdout=res.stdout)


def _log(line: str, frames: int = MIN_FRAMES) -> str:
    return "".join(f"ts={1000 + i} {line}\n" for i in range(frames))


def _errors(tmp_path, recording, log, src=SRC, fast_metadata=True):
    (tmp_path / "in.bin").write_bytes(src)
    out = tmp_path / "rx.out"
    if recording is not None:
        out.write_bytes(recording)
    if log is not None:
        (tmp_path / "rx.out.info").write_text(log)
    params = {
        "input_file": str(tmp_path / "in.bin"),
        "fast_metadata": fast_metadata,
        "fastmetadata_data_item_type": DIT,
        "fastmetadata_k_bit": K_BIT,
    }
    host = SimpleNamespace(connection=_Connection())
    return st40p_recording_errors(host, params, str(out), MIN_FRAMES)


@pytest.mark.parametrize(
    "src_size, fast_metadata, expected",
    [(13, False, (13, 1)), (300, False, (300, 2)), (65539, True, (65540, 65))],
)
def test_st40p_pass(src_size, fast_metadata, expected):
    assert st40p_pass(src_size, fast_metadata) == expected


@pytest.mark.parametrize(
    "recording",
    [PASS * 3, PASS * 3 + PASS[:5000], PASS * 2 + SRC],
    ids=["three_passes", "cut_at_end", "cut_before_padding"],
)
def test_fast_metadata_recording_passes(tmp_path, recording):
    assert _errors(tmp_path, recording, _log(FMD_LINE)) == []


def _with_byte_changed(data: bytes, at: int) -> bytes:
    return data[:at] + bytes([data[at] ^ 0xFF]) + data[at + 1 :]


@pytest.mark.parametrize(
    "recording",
    [
        SRC * 3,
        (SRC + b"\1") * 3,
        PASS + b"".join(ITEMS[:5] + ITEMS[6:]) + PASS,
        PASS + b"".join([ITEMS[1], ITEMS[0], *ITEMS[2:]]) + PASS,
        b"".join(ITEMS[1:]) + PASS * 2,
        PASS + _with_byte_changed(PASS, 4000) + PASS,
        b"",
        None,
    ],
    ids=[
        "no_padding",
        "non_zero_padding",
        "dropped_item",
        "swapped_items",
        "missed_first_item",
        "changed_byte",
        "empty",
        "missing",
    ],
)
def test_fast_metadata_recording_fails(tmp_path, recording):
    errors = _errors(tmp_path, recording, _log(FMD_LINE))
    assert len(errors) == 1 and "is not" in errors[0], errors


@pytest.mark.parametrize(
    "log, fault",
    [
        (None, "holds 0 frames"),
        ("", "holds 0 frames"),
        (_log(FMD_LINE, MIN_FRAMES - 1), f"holds {MIN_FRAMES - 1} frames"),
        (_log(ANC_LINE), "lack marker 0"),
        (_log(FMD_LINE.replace(f"fmd_dit={DIT}", "fmd_dit=1234567")), "lack"),
        (_log(FMD_LINE.replace("fmd_k_bit=1", "fmd_k_bit=0")), "lack"),
        (_log(FMD_LINE.replace("rtp_marker=0", "rtp_marker=1")), "lack"),
        (
            _log(FMD_LINE)
            + _log(FMD_LINE.replace("seq_discont=0", "seq_discont=1"), 1),
            "1 frames show a lost",
        ),
        (_log(FMD_LINE) + _log("garbage", 1), "1 frames show a lost"),
    ],
    ids=[
        "missing",
        "empty",
        "too_few_frames",
        "no_fmd_fields",
        "wrong_dit",
        "wrong_k_bit",
        "marker_1",
        "seq_discont",
        "garbage_line",
    ],
)
def test_fast_metadata_frame_info_fails(tmp_path, log, fault):
    errors = _errors(tmp_path, PASS * 3, log)
    assert fault in "; ".join(errors), errors


def test_anc_recording_of_the_input_repeated_passes(tmp_path):
    src = b"Hello World!\n"
    # A plain ANC frame carries no DIT or K-bit, and its marker is 1.
    assert _errors(tmp_path, src * 400, _log(ANC_LINE), src, False) == []


def test_anc_recording_with_a_lost_frame_fails(tmp_path):
    src = SRC[:300]
    # TX cuts each pass into a 255 B and a 45 B frame; RX loses one 45 B frame.
    errors = _errors(tmp_path, src + src[:255] + src * 3, _log(ANC_LINE), src, False)
    assert len(errors) == 1 and "is not" in errors[0], errors
