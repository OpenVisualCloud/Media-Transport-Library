from __future__ import annotations

import csv
import json
import os
import re
import shlex
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]


def read_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    lines = iter(path.read_text().splitlines())
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        if not sep or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key.strip()):
            raise ValueError(f"invalid config line in {path}: {raw}")
        logical_value = value
        while True:
            try:
                parsed = shlex.split(logical_value, comments=True)
                break
            except ValueError as error:
                if "No closing quotation" not in str(error):
                    raise ValueError(f"invalid config value in {path}: {raw}") from error
                try:
                    logical_value += "\n" + next(lines).strip()
                except StopIteration as stop:
                    raise ValueError(f"unterminated quoted value in {path}: {raw}") from stop
        values[key.strip()] = parsed[0] if parsed else ""
    return values


def load_config(scenario: str, overrides: list[str], noisy_neighbor: str = "") -> dict[str, str]:
    cfg = read_env(ROOT / "config/lab.env")
    scenario_path = Path(scenario)
    if not scenario_path.is_file():
        scenario_path = ROOT / "scenarios" / f"{scenario}.env"
    if not scenario_path.is_file():
        raise ValueError(f"scenario not found: {scenario}")
    cfg.update(read_env(scenario_path))
    cfg.update({k: v for k, v in os.environ.items() if k.startswith("LAB_")})
    if noisy_neighbor:
        profile_path = Path(noisy_neighbor)
        if not profile_path.is_file():
            profile_path = ROOT / "noisy-neighbors" / f"{noisy_neighbor}.env"
        if not profile_path.is_file():
            raise ValueError(f"noisy-neighbor profile not found: {noisy_neighbor}")
        cfg.update(read_env(profile_path))
        cfg["NOISY_NEIGHBOR_ENABLED"] = "1"
        cfg["NOISY_NEIGHBOR_PROFILE"] = profile_path.stem
    for item in overrides:
        key, sep, value = item.partition("=")
        if not sep:
            raise ValueError(f"override must be KEY=VALUE: {item}")
        cfg[key] = value
    return cfg


def run(args: list[str], *, check: bool = True, input_text: str | None = None) -> str:
    process = subprocess.run(args, input=input_text, text=True, capture_output=True)
    if check and process.returncode:
        raise RuntimeError(f"command failed ({process.returncode}): {' '.join(args)}\n{process.stderr}")
    return process.stdout


def utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def dump_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def write_rows(path: Path, rows: list[dict[str, Any]]) -> None:
    fields = ["category", "metric", "unit", "value", "scope", "session", "role"]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def expand_cpu_spec(spec: str) -> list[int]:
    cpus: list[int] = []
    for part in filter(None, spec.split(",")):
        if "-" in part:
            start, end = map(int, part.split("-", 1))
            cpus.extend(range(start, end + 1))
        else:
            cpus.append(int(part))
    return sorted(set(cpus))


def compact_cpus(cpus: list[int]) -> str:
    if not cpus:
        return "none"
    ranges: list[str] = []
    start = previous = cpus[0]
    for cpu in cpus[1:]:
        if cpu == previous + 1:
            previous = cpu
            continue
        ranges.append(str(start) if start == previous else f"{start}-{previous}")
        start = previous = cpu
    ranges.append(str(start) if start == previous else f"{start}-{previous}")
    return ",".join(ranges)
