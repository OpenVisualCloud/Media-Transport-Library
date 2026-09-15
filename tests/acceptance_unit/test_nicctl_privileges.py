# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""``nicctl.sh`` must be invoked with sudo for every subcommand that writes.

The suite SSHes in as an unprivileged user, so each privileged command carries
its own ``sudo``. ``create_vfs`` did; ``disable_vf``, ``bind_pmd`` and
``bind_kernel`` did not, and their failures were indirect -- ``sriov_numvfs:
Permission denied`` and ``RTNETLINK answers: Operation not permitted``, which
``set -e`` in nicctl.sh turns into a bare exit status. ``disable_vf`` and
``bind_kernel`` then fall through to a destructive PCI remove/rescan;
``bind_pmd`` errors the test outright.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NICCTL_PY = ROOT / "tests/acceptance/common/nicctl.py"

# nicctl.sh's only subcommand that reads without writing. Anything else must be
# invoked with sudo.
UNPRIVILEGED_SUBCOMMANDS = frozenset(("list",))

# `sudo`, with any of its own options, immediately before the nicctl.sh path.
# Anchored deliberately: a bare "is 'sudo' somewhere on this line" test passes for
# `sudo sysctl ... && {self.nicctl} disable_vf`, which runs nicctl.sh unprivileged
# and so reproduces the very failure this module exists to prevent.
SUDO_PREFIX = re.compile(r"sudo(?: -\S+)*\s*$")


def invocations():
    """Yield ``(lineno, subcommand, has_sudo)`` for each nicctl.sh call."""
    for lineno, line in enumerate(NICCTL_PY.read_text().splitlines(), 1):
        # \b excludes the unrelated self.nicctl_objs attribute; the lookahead
        # excludes the `self.nicctl = ...` assignment.
        match = re.search(r"self\.nicctl\b(?!\s*=)(?P<tail>.*)", line)
        if not match:
            continue
        head = line[: match.start()].rstrip(' "{')
        words = match["tail"].strip(' "}{').replace('"', " ").split()
        # A subcommand held in a variable ('{cmd}') is unknown here, so it falls
        # through as privileged -- the conservative default.
        subcommand = words[0].strip("{}") if words else ""
        yield lineno, subcommand, bool(SUDO_PREFIX.search(head))


class SudoTests(unittest.TestCase):
    def test_privileged_subcommands_are_invoked_with_sudo(self):
        missing = [
            f"{NICCTL_PY.name}:{lineno} ({subcommand or '<unparsed>'})"
            for lineno, subcommand, has_sudo in invocations()
            if not has_sudo and subcommand not in UNPRIVILEGED_SUBCOMMANDS
        ]
        self.assertFalse(
            missing,
            "these nicctl.sh invocations write sysfs or run `ip link` but lack "
            "sudo, and the suite connects as an unprivileged user: "
            + ", ".join(missing),
        )

    def test_every_invocation_was_parsed(self):
        """Keeps the test above honest if nicctl.py grows a new call style."""
        parsed = list(invocations())
        # Not the current count: a call site added or dropped is not a defect,
        # a pattern that matches nothing is.
        self.assertTrue(parsed, "found no nicctl.sh invocations at all")
        self.assertTrue(all(subcommand for _, subcommand, _ in parsed), parsed)


if __name__ == "__main__":
    unittest.main()
