from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _fake_lab(tmp_path: Path, plan_text: str) -> tuple[Path, Path, Path]:
    """A throwaway copy of the repo layout with run.sh and summarize.sh stubbed."""
    scripts = tmp_path / "scripts"
    scripts.mkdir(parents=True)
    shutil.copy(ROOT / "scripts/run-campaign.sh", scripts / "run-campaign.sh")

    run_log = tmp_path / "runs.log"
    # `cat >/dev/null` makes the stub greedily drain stdin, which is exactly the
    # failure mode being guarded against: a real run.sh calls ssh and kubectl,
    # and if they inherit the plan on stdin the remaining rows disappear.
    (scripts / "run.sh").write_text(
        "#!/usr/bin/env bash\n"
        "cat >/dev/null\n"
        f"printf '%s\\n' \"$*\" >>{run_log}\n"
    )
    (scripts / "summarize.sh").write_text("#!/usr/bin/env bash\nexit 0\n")
    os.chmod(scripts / "run.sh", 0o755)
    os.chmod(scripts / "summarize.sh", 0o755)

    plan = tmp_path / "campaign.env"
    plan.write_text(plan_text)
    return scripts / "run-campaign.sh", plan, run_log


def test_every_plan_row_runs_even_though_children_read_stdin(tmp_path: Path) -> None:
    campaign, plan, run_log = _fake_lab(
        tmp_path,
        "# a comment line\n"
        "\n"
        "baseline  --streams 12 --rdt-monitor\n"
        "numa-pool --streams 14 --rdt-monitor   # trailing comment\n"
        "pinned    --streams 20 --noisy-neighbor host-a --rdt-control mba-20\n",
    )

    completed = subprocess.run(
        ["bash", str(campaign), str(plan)], text=True, capture_output=True, check=True
    )

    runs = run_log.read_text().splitlines()
    assert runs == [
        "baseline --streams 12 --rdt-monitor",
        "numa-pool --streams 14 --rdt-monitor",
        "pinned --streams 20 --noisy-neighbor host-a --rdt-control mba-20",
    ]
    assert "campaign complete: 3 rows, 0 failed" in completed.stdout


def test_a_failing_row_does_not_abort_the_campaign(tmp_path: Path) -> None:
    campaign, plan, run_log = _fake_lab(
        tmp_path, "baseline --streams 12\nbroken\npinned --streams 20\n"
    )
    (campaign.parent / "run.sh").write_text(
        "#!/usr/bin/env bash\n"
        "cat >/dev/null\n"
        f"printf '%s\\n' \"$*\" >>{run_log}\n"
        '[[ "$1" == "broken" ]] && exit 1\n'
        "exit 0\n"
    )
    os.chmod(campaign.parent / "run.sh", 0o755)

    completed = subprocess.run(
        ["bash", str(campaign), str(plan)], text=True, capture_output=True
    )

    # A long overnight campaign must not lose the rows after a failure, but the
    # campaign as a whole still reports failure.
    assert len(run_log.read_text().splitlines()) == 3
    assert "campaign complete: 3 rows, 1 failed" in completed.stdout
    assert completed.returncode != 0


def test_an_empty_plan_is_an_error(tmp_path: Path) -> None:
    campaign, plan, _ = _fake_lab(tmp_path, "# nothing but comments\n\n")
    completed = subprocess.run(
        ["bash", str(campaign), str(plan)], text=True, capture_output=True
    )
    assert completed.returncode == 2
    assert "no runnable rows" in completed.stderr
