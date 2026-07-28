# SPDX-License-Identifier: BSD-3-Clause
import logging
import os
import pathlib
import subprocess

import pytest
from mfd_common_libs.log_levels import TEST_FAIL, TEST_INFO

ROOT_DIR = pathlib.Path(__file__).resolve().parents[3]
FUZZ_BIN_DIR = ROOT_DIR / "build" / "tests" / "fuzz"
FUZZ_TARGETS = [
    "st40_rx_rtp_fuzz",
    "st40_ancillary_helpers_fuzz",
    "st30_rx_frame_fuzz",
    "st20_rx_frame_fuzz",
    "st22_rx_frame_fuzz",
]
FUZZ_RUNS = int(os.environ.get("MTL_FUZZ_TEST_RUNS", "500000"))


def _run_with_logging(target: str, cmd: list[str]) -> None:
    logging.log(TEST_INFO, "Running %s", " ".join(cmd))
    with subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    ) as proc:
        assert proc.stdout is not None
        for line in proc.stdout:
            logging.log(TEST_INFO, "[%s] %s", target, line.rstrip())
        ret = proc.wait()
    if ret != 0:
        logging.log(TEST_FAIL, "%s exited with code %s", target, ret)
        raise subprocess.CalledProcessError(ret, cmd)


@pytest.mark.nightly
@pytest.mark.parametrize("target", FUZZ_TARGETS)
def test_fuzz_target_full_run(target, tmp_path):
    binary = FUZZ_BIN_DIR / target
    if not binary.exists():
        pytest.skip(f"fuzz target {target} not built (expected {binary})")

    corpus_dir = tmp_path / target
    corpus_dir.mkdir()

    cmd = [str(binary), f"-runs={FUZZ_RUNS}", str(corpus_dir)]
    _run_with_logging(target, cmd)
