#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""
Unit tests for the command construction and output parsing of mtl_mcp_server.

These tests build argv lists, parse captured output, and drive the two test
tools with `_run_rc` replaced by a stub. They start no test binary, need no NIC,
and call no sudo, so they run anywhere:

    .github/mcp/.venv/bin/python -m unittest discover -s .github/mcp -v

stdlib unittest on purpose — it adds no entry to requirements.txt, which is
the file that already broke every MCP tool once by floating a version.
"""

from __future__ import annotations

import inspect
import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import mtl_mcp_server
import mtl_setup_common
from mtl_mcp_server import (
    _LD_PATH_PREFIXES,
    DEFAULT_LD_LIBRARY_PATH,
    REPO_ROOT,
    _build_gtest_cmd,
    _build_noctx_cmd,
    _extract_dpdk_version,
    _parse_noctx_listing,
    _run_noctx_series,
    _sudo_credential_error,
    run_noctx_pf_tests,
    run_noctx_tests,
)

BIN = "/repo/build/tests/KahawaiTest"


class BuildGtestCmd(unittest.TestCase):
    def test_pf_rl_run_at_notice_level_is_the_t05_step3_command(self):
        cmd, err = _build_gtest_cmd(
            BIN,
            p_port="0000:15:00.0",
            r_port="0000:15:00.1",
            gtest_filter="St20p*",
            pacing_way="rl",
            log_level="notice",
        )
        self.assertEqual(err, "")
        self.assertEqual(
            cmd,
            [
                "sudo",
                "-n",
                "env",
                "LD_LIBRARY_PATH=/usr/local/lib/x86_64-linux-gnu",
                BIN,
                "--p_port",
                "0000:15:00.0",
                "--r_port",
                "0000:15:00.1",
                "--auto_start_stop",
                "--log_level",
                "notice",
                "--pacing_way",
                "rl",
                "--gtest_filter=St20p*",
            ],
        )

    def test_log_level_defaults_to_notice_so_the_dpdk_banner_is_never_muted(self):
        cmd, err = _build_gtest_cmd(BIN, p_port="0000:15:00.0", r_port="0000:15:00.1")
        self.assertEqual(err, "")
        self.assertIn("--log_level", cmd)
        self.assertEqual(cmd[cmd.index("--log_level") + 1], "notice")

    def test_loader_path_is_forced_with_sudo_env_not_sudo_dash_e(self):
        cmd, _err = _build_gtest_cmd(BIN, p_port="0000:15:00.0", r_port="0000:15:00.1")
        self.assertEqual(cmd[:3], ["sudo", "-n", "env"])
        self.assertNotIn("-E", cmd)
        self.assertEqual(cmd[3], f"LD_LIBRARY_PATH={DEFAULT_LD_LIBRARY_PATH}")

    def test_sudo_never_waits_on_a_password_prompt(self):
        for ld in (DEFAULT_LD_LIBRARY_PATH, ""):
            with self.subTest(ld_library_path=ld):
                cmd, _err = _build_gtest_cmd(
                    BIN,
                    p_port="0000:15:00.0",
                    r_port="0000:15:00.1",
                    ld_library_path=ld,
                )
                self.assertEqual(cmd[:2], ["sudo", "-n"])

    def test_empty_loader_path_leaves_the_loader_alone(self):
        cmd, err = _build_gtest_cmd(
            BIN,
            p_port="0000:15:00.0",
            r_port="0000:15:00.1",
            ld_library_path="",
        )
        self.assertEqual(err, "")
        self.assertEqual(cmd[:3], ["sudo", "-n", BIN])

    def test_unknown_pacing_way_is_rejected(self):
        cmd, err = _build_gtest_cmd(
            BIN, p_port="0000:15:00.0", r_port="0000:15:00.1", pacing_way="turbo"
        )
        self.assertEqual(cmd, [])
        self.assertIn("pacing_way", err)

    def test_every_pacing_way_the_parser_accepts_is_allowed(self):
        for way in ("auto", "rl", "tsn", "tsc", "ptp", "be"):
            with self.subTest(pacing_way=way):
                _cmd, err = _build_gtest_cmd(
                    BIN, p_port="0000:15:00.0", r_port="0000:15:00.1", pacing_way=way
                )
                self.assertEqual(err, "")

    def test_unknown_log_level_is_rejected(self):
        cmd, err = _build_gtest_cmd(
            BIN, p_port="0000:15:00.0", r_port="0000:15:00.1", log_level="verbose"
        )
        self.assertEqual(cmd, [])
        self.assertIn("log_level", err)

    def test_loader_path_with_shell_metacharacters_is_rejected(self):
        cmd, err = _build_gtest_cmd(
            BIN,
            p_port="0000:15:00.0",
            r_port="0000:15:00.1",
            ld_library_path="/usr/local/lib:$(id)",
        )
        self.assertEqual(cmd, [])
        self.assertIn("ld_library_path", err)

    def test_loader_path_must_be_absolute(self):
        cmd, err = _build_gtest_cmd(
            BIN,
            p_port="0000:15:00.0",
            r_port="0000:15:00.1",
            ld_library_path="usr/local/lib",
        )
        self.assertEqual(cmd, [])
        self.assertIn("ld_library_path", err)

    def test_dma_dev_must_be_a_list_of_bdfs(self):
        cmd, err = _build_gtest_cmd(
            BIN,
            p_port="0000:15:00.0",
            r_port="0000:15:00.1",
            dma_dev="0000:00:01.0,;rm -rf /",
        )
        self.assertEqual(cmd, [])
        self.assertIn("dma_dev", err)

    def test_valid_dma_dev_pair_is_passed_through(self):
        cmd, err = _build_gtest_cmd(
            BIN,
            p_port="0000:15:00.0",
            r_port="0000:15:00.1",
            dma_dev="0000:00:01.0,0000:00:01.1",
        )
        self.assertEqual(err, "")
        self.assertEqual(cmd[cmd.index("--dma_dev") + 1], "0000:00:01.0,0000:00:01.1")

    def test_malformed_port_bdf_is_rejected(self):
        cmd, err = _build_gtest_cmd(BIN, p_port="15:00.0", r_port="0000:15:00.1")
        self.assertEqual(cmd, [])
        self.assertIn("p_port", err)

    def test_gtest_filter_metacharacters_are_rejected(self):
        cmd, err = _build_gtest_cmd(
            BIN,
            p_port="0000:15:00.0",
            r_port="0000:15:00.1",
            gtest_filter="St20p*; id",
        )
        self.assertEqual(cmd, [])
        self.assertIn("gtest_filter", err)

    def test_a_trailing_newline_on_the_filter_is_rejected(self):
        cmd, err = _build_gtest_cmd(
            BIN,
            p_port="0000:15:00.0",
            r_port="0000:15:00.1",
            gtest_filter="St20p*\n",
        )
        self.assertEqual(cmd, [])
        self.assertIn("gtest_filter", err)

    def test_a_trailing_newline_on_a_port_bdf_is_rejected(self):
        cmd, err = _build_gtest_cmd(BIN, p_port="0000:15:00.0\n", r_port="0000:15:00.1")
        self.assertEqual(cmd, [])
        self.assertIn("p_port", err)

    def test_a_bdf_list_rejects_the_whitespace_a_scalar_bdf_rejects(self):
        """The list and scalar validators must agree on what a BDF is.

        Accepting whitespace here and then emitting the unstripped value put a
        string in argv that the scalar sibling rejects outright.
        """
        cmd, err = _build_gtest_cmd(
            BIN,
            p_port="0000:15:00.0",
            r_port="0000:15:00.1",
            dma_dev="0000:00:01.0\n , 0000:00:01.1",
        )
        self.assertEqual(cmd, [])
        self.assertIn("dma_dev", err)


class BuildNoctxCmd(unittest.TestCase):
    PORTS = "0000:15:01.0,0000:15:01.1,0000:15:01.2,0000:15:01.3"

    def test_listing_command_enumerates_without_running(self):
        cmd, err = _build_noctx_cmd(
            BIN, self.PORTS, "NoCtxTest.*-NoCtxTest.*_pf_*", list_tests=True
        )
        self.assertEqual(err, "")
        self.assertEqual(
            cmd,
            [
                "env",
                f"LD_LIBRARY_PATH={DEFAULT_LD_LIBRARY_PATH}",
                BIN,
                "--no_ctx_tests",
                "--gtest_list_tests",
                f"--port_list={self.PORTS}",
                "--gtest_filter=NoCtxTest.*-NoCtxTest.*_pf_*",
            ],
        )

    def test_only_the_run_step_needs_root_not_the_enumeration(self):
        list_cmd, _e1 = _build_noctx_cmd(
            BIN, self.PORTS, "NoCtxTest.*", list_tests=True
        )
        run_cmd, _e2 = _build_noctx_cmd(BIN, self.PORTS, "NoCtxTest.init_32_queues")
        self.assertNotIn("sudo", list_cmd)
        self.assertEqual(run_cmd[:2], ["sudo", "-n"])

    def test_the_enumeration_still_forces_the_loader_path(self):
        cmd, _err = _build_noctx_cmd(BIN, self.PORTS, "NoCtxTest.*", list_tests=True)
        self.assertEqual(cmd[:2], ["env", f"LD_LIBRARY_PATH={DEFAULT_LD_LIBRARY_PATH}"])

    def test_an_empty_loader_path_leaves_the_enumeration_bare(self):
        cmd, err = _build_noctx_cmd(
            BIN, self.PORTS, "NoCtxTest.*", list_tests=True, ld_library_path=""
        )
        self.assertEqual(err, "")
        self.assertEqual(cmd[0], BIN)

    def test_run_command_names_exactly_one_case(self):
        cmd, err = _build_noctx_cmd(BIN, self.PORTS, "NoCtxTest.init_32_queues")
        self.assertEqual(err, "")
        self.assertEqual(
            cmd,
            [
                "sudo",
                "-n",
                "env",
                f"LD_LIBRARY_PATH={DEFAULT_LD_LIBRARY_PATH}",
                BIN,
                "--no_ctx_tests",
                "--auto_start_stop",
                f"--port_list={self.PORTS}",
                "--gtest_filter=NoCtxTest.init_32_queues",
            ],
        )

    def test_malformed_port_in_the_list_names_the_port_that_failed(self):
        cmd, err = _build_noctx_cmd(BIN, "0000:15:01.0,nope", "NoCtxTest.x")
        self.assertEqual(cmd, [])
        self.assertIn("port_2", err)
        self.assertIn("nope", err)
        self.assertEqual(err.count("Error:"), 1)

    def test_filter_metacharacters_are_rejected(self):
        cmd, err = _build_noctx_cmd(BIN, self.PORTS, "NoCtxTest.x && id")
        self.assertEqual(cmd, [])
        self.assertIn("gtest_filter", err)


class LdLibraryPathTrustBoundary(unittest.TestCase):
    """`ld_library_path` re-adds a loader search path across a sudo transition.

    Syntax alone is not enough: every accepted element must also name a tree the
    caller could only have populated with the privileges the check protects.
    """

    def _accepts(self, ld_library_path: str) -> tuple[list[str], str]:
        return _build_gtest_cmd(
            BIN,
            p_port="0000:15:00.0",
            r_port="0000:15:00.1",
            ld_library_path=ld_library_path,
        )

    def test_world_writable_and_home_trees_are_rejected(self):
        for ld in (
            "/tmp/evil",
            "/dev/shm/x",
            "/var/tmp/evil",
        ):
            with self.subTest(ld_library_path=ld):
                cmd, err = self._accepts(ld)
                self.assertEqual(cmd, [])
                self.assertIn("ld_library_path", err)

    def test_dot_dot_traversal_out_of_an_allowed_prefix_is_rejected(self):
        cmd, err = self._accepts("/usr/local/lib/../../../tmp/evil")
        self.assertEqual(cmd, [])
        self.assertIn("ld_library_path", err)

    def test_dot_dot_is_rejected_even_when_it_stays_inside_the_allowlist(self):
        cmd, err = self._accepts("/usr/local/lib/x86_64-linux-gnu/../lib")
        self.assertEqual(cmd, [])
        self.assertIn("ld_library_path", err)

    def test_one_untrusted_element_rejects_the_whole_list(self):
        cmd, err = self._accepts(f"/tmp/evil:{DEFAULT_LD_LIBRARY_PATH}")
        self.assertEqual(cmd, [])
        self.assertIn("ld_library_path", err)

    def test_a_sibling_of_an_allowed_prefix_does_not_inherit_its_trust(self):
        cmd, err = self._accepts("/usr/local/libevil")
        self.assertEqual(cmd, [])
        self.assertIn("ld_library_path", err)

    def test_the_default_is_inside_the_allowlist(self):
        _cmd, err = self._accepts(DEFAULT_LD_LIBRARY_PATH)
        self.assertEqual(err, "")

    def test_two_allowed_trees_can_be_joined_for_an_ab_run(self):
        _cmd, err = self._accepts(f"{REPO_ROOT}/dpdk/lib:{DEFAULT_LD_LIBRARY_PATH}")
        self.assertEqual(err, "")

    def test_every_prefix_carries_its_own_justification(self):
        """A prefix appended later does not inherit the argument for these three.

        Each is writable only by the operator who already drives the sudo the
        loader path crosses; a tree that is not must not be added silently.
        """
        justified = {
            "/usr/local/lib": "root-owned; writing it already needs the same sudo",
            str(REPO_ROOT): "the checkout whose KahawaiTest run_gtest execs as root",
            str(
                REPO_ROOT.parent / "Media-Transport-Library" / ".local_install"
            ): "the sibling checkout's install tree, owned by the same operator",
        }
        self.assertEqual(
            set(_LD_PATH_PREFIXES),
            set(justified),
            "every prefix needs a recorded justification and every justification "
            "needs a prefix; state why the operator who can write a new tree "
            "already holds the privilege this check guards, and drop the entry "
            "when its prefix goes",
        )

    def test_the_sibling_install_tree_is_accepted_on_its_own(self):
        sibling = str(REPO_ROOT.parent / "Media-Transport-Library" / ".local_install")
        _cmd, err = self._accepts(f"{sibling}/lib/x86_64-linux-gnu")
        self.assertEqual(err, "")

    def test_a_trailing_newline_does_not_smuggle_a_second_line_past_the_anchor(self):
        cmd, err = self._accepts(f"{DEFAULT_LD_LIBRARY_PATH}\n/tmp/evil")
        self.assertEqual(cmd, [])
        self.assertIn("ld_library_path", err)

    def test_a_trailing_slash_on_an_allowed_directory_is_accepted(self):
        _cmd, err = self._accepts(f"{DEFAULT_LD_LIBRARY_PATH}/")
        self.assertEqual(err, "")

    def test_a_path_too_long_for_execve_is_rejected_before_exec(self):
        long_but_allowlisted = f"{DEFAULT_LD_LIBRARY_PATH}/{'x' * 5000}"
        cmd, err = self._accepts(long_but_allowlisted)
        self.assertEqual(cmd, [])
        self.assertIn("ld_library_path", err)
        self.assertNotIn("x" * 5000, err)


class SudoCredentialFailure(unittest.TestCase):
    """`sudo -n` refusal must be named as such, not as an empty test listing."""

    def test_a_refused_sudo_is_named_as_a_credential_failure(self):
        err = _sudo_credential_error("sudo: a password is required\n")
        self.assertIn("a password is required", err)
        self.assertIn("credential", err.lower())

    def test_no_refusal_that_can_stand_alone_is_left_unrecognised(self):
        """Fixture is the sudo 1.9.15p5 catalogue with `%s` filled in.

        Line numbers are `strings -a /usr/libexec/sudo/sudoers.so`, 3994 lines.
        A regression pin against narrowing, not a gap: the classifier already
        matches every fixture line, so the record below carries the gap argument.

        The PAM block is every message the one `pam_acct_mgmt` call site can
        print, resolved with `objdump -d` rather than inferred: the bounds check
        at 0xe494 admits rc 0..27, the table at .rodata 0x73780 holds 28 int32
        entries over 7 targets, and those emit 6 wordings. :2827 is both the
        `default` (20 codes) and the rc 6/9/11 arm, so 23 of 28 codes reach it.

        :2823 and :2825 were already matched by the `password (?:is )?expired`
        clause, and :3131 by the clause :3259 pins, so all three are regression
        pins rather than new coverage. The fixture is the sweep's whole matched
        set: 12 of the catalogue's 3994 lines.

        Two credential-phase entries stay unmatched, for different reasons:

        * :2824 (`unable to change expired password`) — **proven** never to stand
          alone. Its address 0x6cce8 has exactly one reference in the object, at
          0xe5b4, and nothing branches into 0xe5a3..0xe5b4 from outside; the only
          path there emits :2823 at 0xe56e first, which this test covers.
        * :3269 (`sudoers specifies that root is not allowed to sudo`) — needs
          euid root. An **assumption** about how this server is launched, not
          something the code enforces.

        Over the one boundary measured exactly — the `pam_acct_mgmt` switch — the
        classifier is complete, the sixth of its six wordings being :2824 above.
        Sudo's other refusal classes are unmatched by design and sit outside that
        count. Sudoers configuration is one: :3123, :2414, :3182, :3183 and :3330
        each reach stderr through `sudo_warn_gettext_v1` into
        `sudo_warnx_nodebug_v1`, by a chain sweep that under-reports, so read
        five as a floor; :2715 (`problem parsing sudoers`) reaches no warn at
        all, its one load at 0x600dd being `dcgettext`ed into eventlog's
        `mail_parse_errors` buffer. Option-policy refusals such as :3278 are
        another, and need an option this server never passes. Matching either
        class would trade a missed refusal for a misreported test failure, and a
        sudoers file that does not parse fails every case of a series whether a
        clause names it or not.
        """
        for line in (
            "sudo: a password is required",  # sudoers.so:2708
            "sudo: operator is not in the sudoers file.",  # :3258
            "Sorry, user operator may not run sudo on build01.",  # :3261
            "Sorry, user operator is not allowed to execute '/bin/ls' as root on build01.",  # :3262
            "sudo: sorry, you must have a tty to run sudo",  # :3275
            "sudo: labrat is not allowed to run sudo on build01.",  # :3259
            "sudo: User labrat is not allowed to run sudo on build01.",  # :3131
            "sudo: Account or password is expired, reset your password and try again",  # :2823
            "sudo: Password expired, contact your system administrator",  # :2825
            'sudo: Account expired or PAM config lacks an "account" section for sudo,'
            " contact your system administrator",  # :2826
            # 0xe4b0 default + 0xe4f0 (rc 6, 9, 11) — 23 of the 28 codes
            "sudo: PAM account management error: Permission denied",  # :2827
            "sudo: account validation failure, is your account locked?",  # :2822, rc 7
        ):
            with self.subTest(line=line):
                self.assertNotEqual(_sudo_credential_error(f"{line}\n"), "")

    def test_a_pam_account_refusal_does_not_prescribe_warming_the_cache(self):
        """:2827 also carries PAM_SERVICE_ERR and PAM_SYSTEM_ERR, which no
        credential cache can clear, so the remedy must not name one."""
        err = _sudo_credential_error(
            "sudo: PAM account management error: System error\n"
        )
        self.assertIn("PAM account check", err)
        self.assertNotIn("Warm the sudo", err)

    def test_an_expiry_that_demands_a_reset_does_not_prescribe_warming_the_cache(self):
        """:2823, rc 12 — a warm cache cannot reset the password sudo asks for."""
        err = _sudo_credential_error(
            "sudo: Account or password is expired, reset your password and try"
            " again\n"
        )
        self.assertIn("PAM account check", err)
        self.assertNotIn("Warm the sudo", err)

    def test_an_expired_password_does_not_prescribe_warming_the_cache(self):
        """:2825, rc 27 — only the administrator named in the wording can clear it."""
        err = _sudo_credential_error(
            "sudo: Password expired, contact your system administrator\n"
        )
        self.assertIn("PAM account check", err)
        self.assertNotIn("Warm the sudo", err)

    def test_an_expired_account_does_not_prescribe_warming_the_cache(self):
        """:2826, rc 13 — the account is gone, so no credential for it can be cached."""
        err = _sudo_credential_error(
            'sudo: Account expired or PAM config lacks an "account" section for'
            " sudo, contact your system administrator\n"
        )
        self.assertIn("PAM account check", err)
        self.assertNotIn("Warm the sudo", err)

    def test_a_missing_credential_still_prescribes_warming_the_cache(self):
        """The counterpart: a real credential refusal keeps the remedy."""
        self.assertIn(
            "Warm the sudo", _sudo_credential_error("sudo: a password is required\n")
        )

    def test_a_stale_hosts_warning_beside_a_loader_failure_is_not_a_refusal(self):
        """The `sudo: ` prefix fronts this warning, so only the wording can reject it.

        The binary never started, so nothing was killed and no case reported
        starting. Misreading it abandons the other 27 cases of a series and
        prescribes warming a credential cache that is already warm.
        """
        out = f"{ToolHarness.STALE_HOST}\n{NoctxEnumeration.LOADER_FAILURE}"
        self.assertEqual(_sudo_credential_error(out), "")

    def test_a_stale_hosts_warning_beside_an_eal_failure_is_not_a_refusal(self):
        """Same shape as the loader failure, for a binary that dies inside EAL."""
        out = f"{ToolHarness.STALE_HOST}\nEAL: FATAL: Cannot init memory\n"
        self.assertEqual(_sudo_credential_error(out), "")

    def test_a_real_listing_is_not_mistaken_for_a_credential_failure(self):
        listing = (
            "Note: Google Test filter = NoCtxTest.*\nNoCtxTest.\n  init_32_queues\n"
        )
        self.assertEqual(_sudo_credential_error(listing), "")

    def test_a_bare_mention_of_sudo_is_not_a_refusal(self):
        """A refusal is a wording; naming the program is not one."""
        out = "MTL: cannot run sudo: see doc/run.md\n"
        self.assertEqual(_sudo_credential_error(out), "")

    def test_a_crashed_case_is_not_blamed_on_sudo(self):
        """A process that reached `[ RUN` authenticated; the crash is the news."""
        out = (
            "sudo: unable to resolve host build01: Name or service not known\n"
            "[ RUN      ] NoCtxTest.st30p_redundant_latency\n"
            "EAL: Cannot init memory\n"
            "Segmentation fault\n"
        )
        self.assertEqual(_sudo_credential_error(out), "")

    def test_a_bare_stale_hosts_warning_is_not_a_refusal(self):
        """The warning alone, with nothing else in the output to vouch for it."""
        out = "sudo: unable to resolve host build01: Name or service not known\n"
        self.assertEqual(_sudo_credential_error(out), "")

    def test_a_stale_hosts_warning_does_not_discard_a_passing_run(self):
        out = (
            "sudo: unable to resolve host build01: Name or service not known\n"
            "[ RUN      ] NoCtxTest.init_32_queues\n"
            "[  PASSED  ] 1 test.\n"
        )
        self.assertEqual(_sudo_credential_error(out), "")

    def test_a_stale_hosts_warning_does_not_mask_a_real_test_failure(self):
        out = (
            "sudo: unable to resolve host build01: Name or service not known\n"
            "[  FAILED  ] 1 test, listed below:\n"
        )
        self.assertEqual(_sudo_credential_error(out), "")

    def test_a_timed_out_run_is_not_blamed_on_sudo(self):
        """A hang before `[ RUN` prints nothing but the marker and the warning."""
        out = (
            "sudo: unable to resolve host build01: Name or service not known\n"
            "\n*** TIMEOUT after 600s ***"
        )
        self.assertEqual(_sudo_credential_error(out), "")

    def test_a_timeout_outranks_a_refusal_printed_in_the_same_output(self):
        """A killed run is what the caller must act on first, so the kill wins
        even when a refusal wording sits in the same output."""
        out = "sudo: a password is required\n\n*** TIMEOUT after 600s ***"
        self.assertEqual(_sudo_credential_error(out), "")

    def test_the_timeout_marker_is_the_one_run_rc_actually_writes(self):
        """The classification keys on the marker, not on the rc sentinel."""
        self.assertIn(
            mtl_mcp_server._TIMEOUT_MARKER,
            inspect.getsource(mtl_setup_common._run_rc),
        )


class NoctxSeriesSignature(unittest.TestCase):
    def test_the_two_second_counts_cannot_be_passed_positionally(self):
        params = inspect.signature(_run_noctx_series).parameters
        for name in ("timeout_seconds", "cooldown_seconds"):
            with self.subTest(parameter=name):
                self.assertEqual(params[name].kind, inspect.Parameter.KEYWORD_ONLY)

    def test_each_tool_waits_as_long_as_the_script_it_mirrors(self):
        """Read the cooldown out of each script, so a change there fails here."""
        noctx = REPO_ROOT / "tests/integration_tests/noctx"
        scripts = (
            (run_noctx_tests, noctx / "run.sh", r"^sleep_time=(\d+)$"),
            (run_noctx_pf_tests, noctx / "run_pf.sh", r"^\s*sleep (\d+)$"),
        )
        for tool, script, pattern in scripts:
            with self.subTest(tool=tool.__name__):
                match = re.search(pattern, script.read_text(), re.MULTILINE)
                self.assertIsNotNone(match, f"no cooldown found in {script}")
                default = inspect.signature(tool).parameters["cooldown_seconds"].default
                self.assertEqual(default, int(match.group(1)))


class ParseNoctxListing(unittest.TestCase):
    """Fixture is `KahawaiTest --no_ctx_tests --gtest_list_tests` as `_run_rc` returns it.

    The 11 unindented lines before the suite block and the 8 after it are the
    point: the fixture only proves the parser if it carries the diagnostics a
    real enumeration prints around the one block it must pick out.
    """

    def test_only_the_noctx_cases_are_taken_from_a_real_enumeration(self):
        names = _parse_noctx_listing(NOCTX_LISTING, pf_only=False)
        self.assertEqual(len(names), 28)
        self.assertEqual(names[0], "st30p_redundant_latency")
        self.assertEqual(names[-1], "init_asymmetric_queues_rx_heavy")

    def test_no_preamble_or_diagnostic_line_becomes_a_case_name(self):
        for name in _parse_noctx_listing(NOCTX_LISTING, pf_only=False):
            with self.subTest(name=name):
                self.assertNotIn(" ", name)
                self.assertFalse(name.startswith("MTL:"))

    def test_pf_only_keeps_exactly_what_run_pf_sh_selects(self):
        self.assertEqual(
            _parse_noctx_listing(NOCTX_LISTING, pf_only=True),
            [
                "st20p_tx_epoch_onward_recovers_after_ptp_step_pf_tsn_pacing",
                "st20p_tx_packets_are_spread_over_frame_pf_tsn_pacing",
            ],
        )

    def test_a_foreign_suite_block_does_not_leak_its_cases(self):
        mixed = f"{NOCTX_LISTING}St20pTest.\n  digest_1080p\n"
        self.assertNotIn("digest_1080p", _parse_noctx_listing(mixed, pf_only=False))

    def test_an_empty_listing_yields_no_cases_rather_than_a_blank_name(self):
        self.assertEqual(_parse_noctx_listing("", pf_only=False), [])


class ToolHarness(unittest.TestCase):
    """Drives `run_gtest` and `_run_noctx_series` with `_run_rc` stubbed.

    No binary is started, so this stays in the no-subprocess tier while still
    pinning the real call sites, the return codes they must consult, and the log
    that has to survive an aborted run. Holds no test of its own.
    """

    REFUSAL = "sudo: a password is required"
    STALE_HOST = "sudo: unable to resolve host build01: Name or service not known"
    PORTS = ("0000:15:01.0", "0000:15:01.1", "0000:15:01.2", "0000:15:01.3")

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        fake_repo = Path(tmp.name)
        (fake_repo / "build/tests").mkdir(parents=True)
        (fake_repo / "build/tests/KahawaiTest").write_text("")
        self.saved_logs: dict[str, str] = {}
        self.listing = (0, NOCTX_LISTING)
        self.listing_calls = 0

        def save_test_log(name: str, content: str) -> Path:
            self.saved_logs[name] = content
            return Path(f"/log/{name}")

        stubs = {
            "REPO_ROOT": fake_repo,
            # Port auto-discovery would otherwise shell out to dpdk-devbind.py.
            "_run_output": lambda *_a, **_k: "",
            "_summarize_output": (
                lambda name, out, **kw: f"<<log {name} {len(out)} rc={kw.get('rc')}>>"
            ),
            "_save_test_log": save_test_log,
        }
        for attr, stub in stubs.items():
            patcher = mock.patch.object(mtl_mcp_server, attr, stub)
            patcher.start()
            self.addCleanup(patcher.stop)

    def _gtest(self, rc: int, out: str) -> str:
        with mock.patch.object(mtl_mcp_server, "_run_rc", return_value=(rc, out)):
            return mtl_mcp_server.run_gtest(p_port=self.PORTS[0], r_port=self.PORTS[1])

    def _series(
        self,
        *outcomes: tuple[int, str],
        listing: tuple[int, str] | None = None,
        pf_only: bool = False,
    ) -> str:
        """Drive the series on a stubbed enumeration plus one outcome per case.

        The stub dispatches on argv, not on call order, so the enumeration keeps
        its own return code however the tool chooses to fetch it. `listing_calls`
        keeps what call order gave for free: a second enumeration is a second
        60-second subprocess on a real host.
        """
        if listing is not None:
            self.listing = listing
        cases = iter(outcomes)

        def run_rc(cmd: list[str], **_kw: object) -> tuple[int, str]:
            if "--gtest_list_tests" in cmd:
                self.listing_calls += 1
                return self.listing
            return next(cases)

        with mock.patch.object(mtl_mcp_server, "_run_rc", run_rc):
            return mtl_mcp_server._run_noctx_series(
                list(self.PORTS),
                "NoCtxTest.*",
                timeout_seconds=1,
                cooldown_seconds=0,
                ld_library_path=DEFAULT_LD_LIBRARY_PATH,
                pf_only=pf_only,
            )


class NoctxEnumeration(ToolHarness):
    """An enumeration the binary never produced must not read as an empty suite."""

    LOADER_FAILURE = (
        "KahawaiTest: error while loading shared libraries: libmtl.so.0: "
        "cannot open shared object file: No such file or directory"
    )

    def test_a_failed_enumeration_is_not_reported_as_nothing_to_do(self):
        """Exit 127 and a VF-only host both list zero `_pf_` cases.

        Only the return code separates them, so the headline must come from it
        and not from the listing text.
        """
        result = self._series(listing=(127, self.LOADER_FAILURE), pf_only=True)
        self.assertIn("127", result)
        self.assertIn("libmtl.so.0", result)
        self.assertNotIn("No PF-only", result)

    def test_a_vf_only_host_listing_no_pf_cases_is_still_not_an_error(self):
        """Keeps the real enumeration's diagnostics, which say `Error:` eight times.

        So the headline is the only thing that can carry the verdict — a bare
        `Error` substring search would pass on a stripped fixture and fail here.
        """
        vf_only = "\n".join(
            line for line in NOCTX_LISTING.splitlines() if "_pf_" not in line
        )
        result = self._series(listing=(0, vf_only), pf_only=True)
        self.assertIn("No PF-only", result)
        self.assertFalse(result.startswith("Error"))

    def test_an_enumeration_that_timed_out_is_not_reported_as_exit_minus_one(self):
        """rc -1 is the harness sentinel, not an exit code the binary produced."""
        result = self._series(listing=(-1, "\n*** TIMEOUT after 60s ***"))
        self.assertIn("timed out", result)
        self.assertNotIn("exit -1", result)

    def test_the_enumeration_is_fetched_once_for_a_whole_series(self):
        self._series(*[(0, "[  PASSED  ] 1 test.\n")] * 28)
        self.assertEqual(self.listing_calls, 1)


class SudoCredentialWiring(ToolHarness):
    """Both test tools must consult `_sudo_credential_error` and keep the output."""

    def test_run_gtest_names_a_refused_sudo_rather_than_counting_zero_tests(self):
        result = self._gtest(1, f"{self.REFUSAL}\n")
        self.assertIn("credential failure", result)
        self.assertNotIn("Total: 0", result)

    def test_run_gtest_keeps_the_output_it_captured_when_sudo_is_refused(self):
        """The log marker alone proves nothing — the normal path emits it too."""
        result = self._gtest(1, f"{self.REFUSAL}\n")
        self.assertLess(result.index("credential failure"), result.index("<<log gtest"))

    def test_run_gtest_marks_a_crashed_binary_failed_rather_than_zero_tests(self):
        self.assertIn("rc=139", self._gtest(139, "EAL: Cannot init memory\n"))

    def test_run_gtest_reports_a_run_that_sudo_allowed_as_a_run(self):
        result = self._gtest(0, "[  PASSED  ] 42 tests.\n")
        self.assertNotIn("credential", result)
        self.assertIn("Passed: 42", result)

    def test_the_series_names_a_refused_sudo_rather_than_calling_it_a_failure(self):
        result = self._series((1, f"{self.REFUSAL}\n"))
        self.assertIn("credential failure", result)

    def test_a_refusal_mid_series_keeps_the_cases_that_already_ran(self):
        result = self._series(
            (0, "[  PASSED  ] 1 test.\n"),
            (0, "[  PASSED  ] 1 test.\n"),
            (1, f"{self.REFUSAL}\n"),
        )
        self.assertIn("credential failure", result)
        self.assertIn("Status: FAILED", result)
        self.assertIn("Aborted after 2 of 28 cases", result)
        self.assertIn("- PASS: NoCtxTest.st30p_redundant_latency", result)
        self.assertIn(
            "===== NoCtxTest.st30p_redundant_latency2: PASS =====",
            self.saved_logs["noctx"],
        )

    def test_the_aborting_case_keeps_the_diagnostics_that_explain_the_abort(self):
        result = self._series((1, f"{self.REFUSAL}\nEAL: Cannot init memory\n"))
        self.assertIn("credential failure", result)
        self.assertIn("EAL: Cannot init memory", self.saved_logs["noctx"])

    def test_a_case_name_the_filter_cannot_carry_aborts_instead_of_raising(self):
        """A value-parameterised listing annotates a name with `# GetParam()`.

        That aborts at the argv check, before `_run_rc`, so the abort path must
        not read output a run never produced.
        """
        listing = "NoCtxTest.\n  init_32_queues/0  # GetParam() = 4\n"
        result = self._series(listing=(0, listing))
        self.assertIn("invalid gtest_filter", result)
        self.assertIn("Aborted after 0 of 1 cases", result)

    def test_an_abort_before_any_case_ran_does_not_lecture_about_build_mode(self):
        result = self._series((1, f"{self.REFUSAL}\n"))
        self.assertNotIn("MTL_SIMULATE_PACKET_DROPS", result)

    def test_a_case_that_printed_pass_but_died_is_not_counted_as_a_pass(self):
        result = self._series(
            (139, "[  PASSED  ] 1 test.\n"),
            *[(0, "[  PASSED  ] 1 test.\n")] * 27,
        )
        self.assertIn("- FAIL: NoCtxTest.st30p_redundant_latency", result)

    def test_a_case_that_died_early_on_a_stale_hosts_host_does_not_abort_the_series(
        self,
    ):
        """The blocker's cost: one misread warning discards the other 27 cases."""
        result = self._series(
            (127, f"{self.STALE_HOST}\n{NoctxEnumeration.LOADER_FAILURE}"),
            *[(0, "[  PASSED  ] 1 test.\n")] * 27,
        )
        self.assertNotIn("credential", result)
        self.assertNotIn("Aborted after", result)
        self.assertIn("Tests run: 28 (passed 27, failed 1)", result)

    def test_run_gtest_reports_a_timeout_rather_than_an_empty_run(self):
        """Runs the real summarizer, because the stub cannot omit a result line.

        rc -1 is the harness sentinel for a kill, so the call must pass no `rc`
        at all: any verdict line would either print the sentinel as an exit code
        or, for `rc=0`, report the kill as OK and suppress the log tail the
        headline tells the reader to consult.
        """
        with mock.patch.object(
            mtl_mcp_server, "_summarize_output", mtl_setup_common._summarize_output
        ), mock.patch.object(
            mtl_setup_common, "_save_test_log", lambda name, content: Path("/log/gtest")
        ):
            result = self._gtest(-1, f"{self.STALE_HOST}\n\n*** TIMEOUT after 600s ***")
        self.assertNotIn("credential", result)
        self.assertIn("TIMEOUT", result)
        self.assertNotIn("Total: 0", result)
        self.assertNotIn("**Result:", result)

    def test_the_result_label_is_the_one_the_shared_summariser_writes(self):
        """Pins the label the test above keys on, which lives in a module this
        suite does not own; a rename there would revive the mutant silently."""
        self.assertIn(
            "**Result:", inspect.getsource(mtl_setup_common._summarize_output)
        )

    def test_a_timeout_before_any_case_started_does_not_prescribe_a_longer_budget(self):
        """A sudoers lookup that hangs in sssd is killed with no case started."""
        result = self._gtest(-1, f"{self.STALE_HOST}\n\n*** TIMEOUT after 600s ***")
        self.assertIn("may not have started", result)
        self.assertNotIn("Raise `timeout_seconds`", result)

    def test_a_timeout_after_a_case_started_prescribes_a_longer_budget(self):
        out = "[ RUN      ] St20pTest.digest\n\n*** TIMEOUT after 600s ***"
        self.assertIn("Raise `timeout_seconds`", self._gtest(-1, out))

    def test_a_timed_out_case_is_recorded_and_the_series_continues(self):
        result = self._series(
            (-1, f"{self.STALE_HOST}\n\n*** TIMEOUT after 1s ***"),
            *[(0, "[  PASSED  ] 1 test.\n")] * 27,
        )
        self.assertNotIn("credential", result)
        self.assertIn("- TIMEOUT: NoCtxTest.st30p_redundant_latency", result)
        self.assertIn("Tests run: 28 (passed 27, failed 1)", result)

    def test_a_series_sudo_never_refused_reports_every_case(self):
        result = self._series(*[(0, "[  PASSED  ] 1 test.\n")] * 28)
        self.assertNotIn("credential", result)
        self.assertIn("Tests run: 28 (passed 28, failed 0)", result)


# Captured from `KahawaiTest --no_ctx_tests --gtest_list_tests`. Split by stream
# because the tool never sees them interleaved: _run_rc returns stdout, a
# newline, then stderr — so the MTL diagnostics land *after* the case list, and
# only indentation can still tell a case name from a diagnostic.
NOCTX_STDOUT = (
    "test_parse_port_list, port list "
    "0000:c9:01.0,0000:c9:01.1,0000:c9:01.2,0000:c9:01.3\n"
    "next_port: 0000:c9:01.0\n"
    "next_port: 0000:c9:01.1\n"
    "next_port: 0000:c9:01.2\n"
    "next_port: 0000:c9:01.3\n"
    "run_all_test, if ip 197.163.151.1 for port 0000:c9:01.0\n"
    "run_all_test, if ip 197.163.151.198 for port 0000:c9:01.1\n"
    "run_all_test, if ip 197.163.151.199 for port 0000:c9:01.2\n"
    "run_all_test, if ip 197.163.151.200 for port 0000:c9:01.3\n"
    "st_test_st22_plugin_register, decoder register fail\n"
    "st_test_convert_plugin_register, converter register fail\n"
    "NoCtxTest.\n"
    "  st30p_redundant_latency\n"
    "  st30p_redundant_latency2\n"
    "  st30p_default_timestamps\n"
    "  st30p_user_pacing\n"
    "  st20p_redundant_latency_drops_even_odd\n"
    "  st20p_default_timestamps\n"
    "  st20p_user_pacing\n"
    "  st20p_user_pacing_offset_jitter\n"
    "  st20p_exact_user_pacing\n"
    "  st20p_tx_multithread_stability\n"
    "  st20p_tx_epoch_onward_recovers_after_ptp_step_pf_tsn_pacing\n"
    "  st20p_tx_epoch_onward_recovers_after_ptp_step_tsc_pacing\n"
    "  st20p_tx_packets_are_spread_over_frame_pf_tsn_pacing\n"
    "  st40i_smoke\n"
    "  st40i_split_flag_accepts_and_propagates\n"
    "  st40i_split_multi_packet_roundtrip\n"
    "  st40i_split_loopback\n"
    "  st40i_split_seq_gap_reports_loss\n"
    "  st40p_rx_auto_detect_interlace\n"
    "  st40p_user_pacing\n"
    "  st40p_user_pacing_59fps\n"
    "  st40p_user_pacing_offset_jitter\n"
    "  st40p_exact_user_pacing\n"
    "  init_32_queues\n"
    "  init_64_queues\n"
    "  init_128_queues\n"
    "  init_asymmetric_queues_tx_heavy\n"
    "  init_asymmetric_queues_rx_heavy\n"
)

NOCTX_STDERR = (
    "MTL: 2026-08-25 11:55:55, Error: mtl_port_ip_info, null handle\n"
    "MTL: 2026-08-25 11:55:55, Error: mtl_port_ip_info, null handle\n"
    "MTL: 2026-08-25 11:55:55, Error: mtl_port_ip_info, null handle\n"
    "MTL: 2026-08-25 11:55:55, Error: mtl_port_ip_info, null handle\n"
    "MTL: 2026-08-25 11:55:55, Error: mtl_iova_mode_get, null handle\n"
    "MTL: 2026-08-25 11:55:55, Error: mtl_rss_mode_get, null handle\n"
    "MTL: 2026-08-25 11:55:55, Error: st22_decoder_register, invalid impl\n"
    "MTL: 2026-08-25 11:55:55, Error: st20_converter_register, null handle\n"
)

NOCTX_LISTING = f"{NOCTX_STDOUT}\n{NOCTX_STDERR}"


class ExtractDpdkVersion(unittest.TestCase):
    def test_banner_line_yields_the_loaded_dpdk_version(self):
        out = (
            "MTL: mtl_init, MTL version: 25.06.0, dpdk version: DPDK 26.07.0_mtl_0\n"
            "[  PASSED  ] 42 tests.\n"
        )
        self.assertEqual(_extract_dpdk_version(out), "DPDK 26.07.0_mtl_0")

    def test_muted_banner_yields_nothing_rather_than_a_guess(self):
        self.assertEqual(_extract_dpdk_version("[  PASSED  ] 42 tests.\n"), "")


if __name__ == "__main__":
    unittest.main()
