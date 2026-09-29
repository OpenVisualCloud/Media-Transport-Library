# SPDX-License-Identifier: BSD-3-Clause
"""Run each libFuzzer target of ``tests/fuzz`` as one pytest case.

The test reads the target names from ``tests/fuzz/meson.build``, so a new
harness gets a case with no change here. The test skips a target that is not
built.
"""

import logging
import os
import pathlib
import re
import subprocess

import pytest
from mfd_common_libs.log_levels import TEST_FAIL, TEST_INFO

ROOT_DIR = pathlib.Path(__file__).resolve().parents[3]
FUZZ_BIN_DIR = (
    pathlib.Path(os.environ.get("MTL_FUZZ_BUILD_DIR", ROOT_DIR / "build"))
    / "tests"
    / "fuzz"
)
# The ['name', 'dir/file.c'] entries of the fuzz_targets list.
FUZZ_TARGETS = re.findall(
    r"\[\s*'(\w+)'\s*,\s*'[^']+\.c'\s*\]",
    (ROOT_DIR / "tests" / "fuzz" / "meson.build").read_text(encoding="utf-8"),
)
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
    """Run one fuzz target for ``MTL_FUZZ_TEST_RUNS`` inputs.

    The corpus starts empty, in a temporary directory. The case fails when the
    target exits with a code that is not zero, for example on a crash or on a
    sanitizer finding. Each output line of the target goes to the test log.

    :param target: Name of the fuzz target, from ``tests/fuzz/meson.build``.
    :param tmp_path: Temporary directory of the case, for the corpus.
    """
    binary = FUZZ_BIN_DIR / target
    if not binary.exists():
        pytest.skip(f"fuzz target {target} not built (expected {binary})")

    corpus_dir = tmp_path / target
    corpus_dir.mkdir()

    cmd = [str(binary), f"-runs={FUZZ_RUNS}", str(corpus_dir)]
    _run_with_logging(target, cmd)
