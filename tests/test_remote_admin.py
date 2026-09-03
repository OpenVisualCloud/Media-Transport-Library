from __future__ import annotations

import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "scripts/lib/remote-admin.sh"


def _selection(user: str) -> list[str]:
    script = (
        f"source {HELPER}\n"
        f"lab_remote_admin_init {user}\n"
        "printf 'sudo=%s\\n' \"$LAB_REMOTE_SUDO\"\n"
        "printf 'ssh=%s\\n' \"${LAB_REMOTE_SSH[*]}\"\n"
    )
    result = subprocess.run(["bash", "-c", script], text=True, capture_output=True, check=True)
    return result.stdout.splitlines()


def test_root_runs_directly_without_a_tty() -> None:
    assert _selection("root") == ["sudo=", "ssh=ssh -o BatchMode=yes"]


def test_normal_user_gets_sudo_and_a_tty() -> None:
    assert _selection("operator") == ["sudo=sudo", "ssh=ssh -t"]