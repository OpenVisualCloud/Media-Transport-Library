#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
# Writes to $2 the newest perf session history artifact $1 of another run of this repo
# ($3 main: of main only); exits 1 if it cannot tell, as unavailable is not empty.

import io
import json
import os
import sys
import urllib.parse
import urllib.request
import zipfile

API = f"{os.environ['GITHUB_API_URL']}/repos/{os.environ['GITHUB_REPOSITORY']}"


def _get(url: str) -> bytes:
    request = urllib.request.Request(
        url, headers={"Accept": "application/vnd.github+json"}
    )
    # Kept off redirects: the zip's redirect is a signed blob URL, which must not get it.
    request.add_unredirected_header(
        "Authorization", f"Bearer {os.environ['GITHUB_TOKEN']}"
    )
    with urllib.request.urlopen(request, timeout=60) as r:  # nosec B310 # GitHub API
        return r.read()


def _artifacts(name: str) -> list:
    found, page = [], 1
    while True:
        query = urllib.parse.urlencode({"name": name, "per_page": 100, "page": page})
        data = json.loads(_get(f"{API}/actions/artifacts?{query}"))
        found += data["artifacts"]
        if len(found) >= data["total_count"]:
            return found
        if not data["artifacts"]:
            # A short list could miss the history, and this run would then record over it.
            raise RuntimeError(f"page {page} is empty before all are listed")
        page += 1


def _newest(artifacts: list, from_main: bool):
    # A fork's pull request can upload under any name, and a branch's workflow too.
    run_id = int(os.environ["GITHUB_RUN_ID"])
    kept = []
    for a in artifacts:
        run = a.get("workflow_run") or {}
        if (
            not a["expired"]
            and run.get("id") not in (None, run_id)
            and run.get("repository_id") is not None
            and run.get("head_repository_id") == run["repository_id"]
            and (not from_main or run.get("head_branch") == "main")
        ):
            kept.append(a)
    return max(kept, key=lambda a: a["created_at"], default=None)


def main():
    name, file, kind = sys.argv[1:4]
    try:
        artifact = _newest(_artifacts(name), kind == "main")
        if artifact:
            with zipfile.ZipFile(
                io.BytesIO(_get(f"{API}/actions/artifacts/{artifact['id']}/zip"))
            ) as z:
                (member,) = z.namelist()
                data = z.read(member)
            os.makedirs(os.path.dirname(file), exist_ok=True)
            with open(file, "wb") as f:
                f.write(data)
        print(f"{name}: run {artifact['workflow_run']['id'] if artifact else 'none'}")
    except Exception as e:
        print(f"::warning::{name}: {e!r}")
        sys.exit(1)


if __name__ == "__main__":
    main()
