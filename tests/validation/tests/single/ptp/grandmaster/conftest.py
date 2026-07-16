# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Fixtures local to tests/single/ptp/grandmaster/ (per-category, not framework)."""
import logging

import pytest

logger = logging.getLogger(__name__)


@pytest.fixture
def require_grandmaster(hosts, ptp_sync):
    """Return the ptp4l log path, already confirmed synced by ``ptp_sync``.

    ``ptp_sync`` itself now blocks in fixture setup until every ptp4l it
    starts has heard a foreign master (see conftest.py), so this fixture no
    longer needs to re-run that wait -- it only resolves and returns the log
    path for callers that want to inspect it further.
    """
    host = list(hosts.values())[0]
    log_path = (
        host.connection.execute_command(
            "ls -t /tmp/ptp4l-*.log 2>/dev/null | head -1", expected_return_codes=None
        ).stdout
        or ""
    ).strip()
    if not log_path:
        raise RuntimeError(
            "ENVIRONMENT NOT READY: ptp_sync did not start ptp4l (check "
            "capture_cfg.enable in test_config.yaml) -- cannot verify a "
            "grandmaster is reachable"
        )
    return log_path
