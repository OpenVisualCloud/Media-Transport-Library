from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

NODES_ENV = """\
# addresses, as a user fills them in
CONTROL_PLANE_HOST=10.0.0.11
WORKER_1_HOST=10.0.0.12   # the Xeon
LAB_SSH_USER=root
"""

CLUSTER = (
    "localhost,127.0.0.1,10.96.0.0/12,10.244.0.0/16,.svc,.cluster.local,"
    "10.0.0.11,control-plane,10.0.0.12,worker-1"
)


def _lib(tmp_path: Path) -> Path:
    """scripts/lib/no-proxy.sh in a throwaway repo layout with its own nodes.env."""
    lib = tmp_path / "scripts/lib"
    lib.mkdir(parents=True)
    shutil.copy(ROOT / "scripts/lib/no-proxy.sh", lib / "no-proxy.sh")
    (tmp_path / "config").mkdir()
    (tmp_path / "config/nodes.env").write_text(NODES_ENV)
    return lib / "no-proxy.sh"


def _run(lib: Path, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    script = f"source {lib}\nlab_export_no_proxy\nprintf '%s' \"${{no_proxy:-}}\"\n"
    # env= replaces the environment entirely, so an unlisted proxy variable is
    # genuinely absent rather than inherited from whoever runs the tests.
    return subprocess.run(
        ["bash", "-c", script], text=True, capture_output=True, env={"PATH": "/usr/bin:/bin"} | env
    )


def test_a_direct_connection_is_left_alone(tmp_path: Path) -> None:
    # No proxy means no_proxy is irrelevant, and setting it would be noise.
    result = _run(_lib(tmp_path), {})
    assert result.stdout == ""
    assert result.stderr == ""


def test_a_proxied_shell_gets_the_whole_cluster_excluded(tmp_path: Path) -> None:
    # The failure this prevents: kubectl asks the proxy to CONNECT to the API
    # server on 6443, the proxy answers 403, kubectl prints "Forbidden".
    result = _run(_lib(tmp_path), {"http_proxy": "http://proxy.example.com:911"})
    assert result.stdout == CLUSTER
    assert "excluding the cluster's own addresses" in result.stderr


def test_only_https_proxy_set_still_counts(tmp_path: Path) -> None:
    result = _run(_lib(tmp_path), {"HTTPS_PROXY": "http://proxy.example.com:912"})
    assert result.stdout == CLUSTER


def test_existing_entries_are_kept(tmp_path: Path) -> None:
    result = _run(
        _lib(tmp_path),
        {"http_proxy": "http://proxy.example.com:911", "no_proxy": ".example.com,169.254.169.254"},
    )
    assert result.stdout == f".example.com,169.254.169.254,{CLUSTER}"


def test_a_complete_no_proxy_is_not_rewritten(tmp_path: Path) -> None:
    result = _run(
        _lib(tmp_path),
        {"http_proxy": "http://proxy.example.com:911", "no_proxy": CLUSTER},
    )
    assert result.stdout == CLUSTER
    assert result.stderr == ""


def test_both_spellings_are_exported(tmp_path: Path) -> None:
    lib = _lib(tmp_path)
    script = f"source {lib}\nlab_export_no_proxy\nprintf '%s' \"$NO_PROXY\"\n"
    result = subprocess.run(
        ["bash", "-c", script],
        text=True,
        capture_output=True,
        env={"PATH": "/usr/bin:/bin", "http_proxy": "http://proxy.example.com:911"},
    )
    assert result.stdout == CLUSTER
