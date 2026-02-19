"""Conftest for performance tests — dynamic marker injection."""

import pytest


def pytest_collection_modifyitems(items):
    """Add ``base_performance`` marker to every 59 fps / 1080p variant.

    These represent the baseline configuration that should always be run
    first (quick sanity) before launching the full nightly matrix.

    Usage::

        pytest -m base_performance          # run only the base subset
        pytest -m "not base_performance"    # skip the base subset
    """
    base_mark = pytest.mark.base_performance
    for item in items:
        node_id = item.nodeid
        if "59fps" in node_id and "1080p" in node_id:
            item.add_marker(base_mark)
