#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

import json
import sys
import unittest
from pathlib import Path
from types import ModuleType


class FastMCP:
    def __init__(self, *_args, **_kwargs):
        pass

    def tool(self):
        return lambda function: function


mcp_module = ModuleType("mcp")
mcp_server_module = ModuleType("mcp.server")
mcp_fastmcp_module = ModuleType("mcp.server.fastmcp")
mcp_fastmcp_module.FastMCP = FastMCP
sys.modules.setdefault("mcp", mcp_module)
sys.modules.setdefault("mcp.server", mcp_server_module)
sys.modules.setdefault("mcp.server.fastmcp", mcp_fastmcp_module)

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mtl_ci_mcp_server as server


class ProductionCiToolsTest(unittest.TestCase):
    def setUp(self):
        self.run_rc = server._run_rc

    def tearDown(self):
        server._run_rc = self.run_rc

    def test_generic_annotation_falls_through_to_log(self):
        def fake_run(command, **_kwargs):
            args = command[1:] if command[0] == "gh" else command
            if args[:2] == ["pr", "view"]:
                return 0, json.dumps({"headRefOid": "abc", "url": "https://example/pr"})
            if args[0] == "api" and "/commits/" in args[-1]:
                return 0, json.dumps(
                    {
                        "check_runs": [
                            {
                                "id": 7,
                                "name": "build",
                                "conclusion": "failure",
                                "details_url": "https://github.com/o/r/actions/runs/42/job/7",
                            }
                        ]
                    }
                )
            if args[0] == "api":
                return 0, json.dumps(
                    [
                        {
                            "annotation_level": "failure",
                            "path": ".github",
                            "start_line": 1,
                            "message": "Process completed with exit code 201.",
                        }
                    ]
                )
            if args[:2] == ["run", "view"] and "--json" in args:
                return 0, json.dumps(
                    {"jobs": [{"steps": [{"name": "Build", "conclusion": "failure"}]}]}
                )
            if args[:2] == ["run", "view"]:
                return (
                    0,
                    "build\tstep\t2026-01-01T00:00:00Z fatal error: asm/types.h not found\n",
                )
            raise AssertionError(args)

        server._run_rc = fake_run
        result = server.ci_pr_failures(1, "o/r")
        self.assertIn("Process completed with exit code 201", result)
        self.assertIn("asm/types.h", result)

    def test_failure_count_and_stderr_are_bounded(self):
        failed = [
            {
                "id": index,
                "name": f"failure-{index}",
                "conclusion": "failure",
                "details_url": f"https://example/check/{index}",
            }
            for index in range(25)
        ]

        def fake_run(command, **_kwargs):
            args = command[1:] if command[0] == "gh" else command
            if args[:2] == ["pr", "view"]:
                return 0, json.dumps({"headRefOid": "abc", "url": "https://example/pr"})
            if args[0] == "api" and "/commits/" in args[-1]:
                return 0, json.dumps({"check_runs": failed})
            if args[0] == "api":
                return 0, "[]"
            raise AssertionError(args)

        server._run_rc = fake_run
        result = server.ci_pr_failures(1, "o/r", max_failures=3)
        self.assertEqual(result.count("#### [failure-"), 3)
        self.assertIn("22 additional failed checks omitted.", result)

        server._run_rc = lambda _command, **_kwargs: (1, "token=ghp_secret")
        _data, error = server._gh_json(["pr", "view"])
        self.assertNotIn("ghp_secret", error)
        self.assertIn("exit 1", error)

    def test_omitted_actionable_annotation_does_not_suppress_log(self):
        annotations = [
            {
                "annotation_level": "failure",
                "path": ".github",
                "start_line": index,
                "message": "Process completed with exit code 1.",
            }
            for index in range(1, 9)
        ]
        annotations.append(
            {
                "annotation_level": "failure",
                "path": "manager/meson.build",
                "start_line": 69,
                "message": "fatal error: asm/types.h not found",
            }
        )

        def fake_run(command, **_kwargs):
            args = command[1:] if command[0] == "gh" else command
            if args[:2] == ["pr", "view"]:
                return 0, json.dumps({"headRefOid": "abc", "url": "https://example/pr"})
            if args[0] == "api" and "/commits/" in args[-1]:
                return 0, json.dumps(
                    {
                        "check_runs": [
                            {
                                "id": 7,
                                "name": "build",
                                "conclusion": "failure",
                                "details_url": "https://github.com/o/r/actions/runs/42/job/7",
                            }
                        ]
                    }
                )
            if args[0] == "api":
                return 0, json.dumps(annotations)
            if args[:2] == ["run", "view"] and "--json" in args:
                return 0, json.dumps({"jobs": []})
            if args[:2] == ["run", "view"]:
                return 0, "build\tstep\tfatal error: asm/types.h not found\n"
            raise AssertionError(args)

        server._run_rc = fake_run
        result = server.ci_pr_failures(1, "o/r", log_lines=8)
        self.assertIn("1 additional annotations omitted", result)
        self.assertIn("asm/types.h", result)


if __name__ == "__main__":
    unittest.main()
