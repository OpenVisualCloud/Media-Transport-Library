"""Make `python/` importable for the tests.

pyproject sets `pythonpath = ["python"]`, but that option only exists in
pytest >= 7 and the lab controller ships an older one. Doing it here keeps
`python3 -m pytest` working on any supported distribution.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
