# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Post-run RX/TX content-integrity checking, owned by one
``IntegritySession`` object -- mirrors ``pcap_compliance.py``'s
``ComplianceSession`` for the compliance check.

The ``media_integrity`` fixture (tests/acceptance/conftest.py) is the single
entry point: it builds an ``IntegritySession`` and hands it to
``execute_test(integrity=media_integrity)``. ``Application._finalize_run()``
calls the session's ``evaluate()`` strictly *before* ``validate_results()``
calls ``Application._cleanup_output_files()`` -- so tests no longer need
``keep_output=True`` to keep the RX file alive long enough for this check.
Tests that don't want an integrity check get ``NO_INTEGRITY``, a no-op
stand-in sharing this module's public surface.

One session handles both video (st20p/st22p) and audio (st30p) sessions --
``IntegrityIntent.kind`` selects which ``common.integrity.integrity_runner``
Runner and which ``mtl_engine.integrity`` size/count helper apply. Actual
frame/sample comparison happens over the host connection (via those
Runners), on whichever host produced the file -- this module only owns
*when* to run it and enforcing it was actually dispatched.
"""

import logging
import os
from dataclasses import dataclass
from typing import TYPE_CHECKING, Optional, Protocol

from common.integrity.integrity_runner import (
    FileAudioIntegrityRunner,
    FileVideoIntegrityRunner,
)

from .execute import log_fail
from .integrity import get_channel_number, get_sample_number, get_sample_size

logger = logging.getLogger(__name__)


@dataclass
class IntegrityIntent:
    """What an ``Application`` hands an ``IntegritySession`` to evaluate.

    ``kind`` ("video" or "audio") selects which of the two field groups
    below ``evaluate()`` reads. Built by ``Application.integrity_intent()``
    from ``self.params`` so this module never reaches into Application
    internals directly -- mirrors ``CaptureIntent`` for compliance.
    """

    host: object
    test_repo_path: Optional[str]
    src_url: Optional[str]
    out_url: Optional[str]
    kind: str
    # video-only
    width: Optional[int] = None
    height: Optional[int] = None
    file_format: Optional[str] = None
    # audio-only
    audio_format: Optional[str] = None
    audio_channels: Optional[list] = None
    audio_sampling: Optional[str] = None
    audio_ptime: Optional[str] = None


class IntegrityCheck(Protocol):
    """Shared surface of ``IntegritySession`` and its null-object stand-in.

    Enforces, at type-check time, that ``_NullIntegritySession`` cannot
    silently drift out of parity with the real session as the surface grows.
    """

    enabled: bool

    def skip(self, reason: str) -> None: ...

    def evaluate(self, intent: IntegrityIntent, fail_on_error: bool = True) -> bool: ...

    def close(self, enforce_dispatch: bool = True) -> None: ...


class IntegritySession:
    """Owns the post-run content-integrity verdict for one test.

    Created only by the ``media_integrity`` fixture
    (tests/acceptance/conftest.py). Runs ``FileVideoIntegrityRunner`` or
    ``FileAudioIntegrityRunner`` (common/integrity/integrity_runner.py),
    whichever ``intent.kind`` selects, against the RX output file on the
    host that produced it, comparing it against the TX source file.
    """

    def __init__(self):
        self._skip_reason: Optional[str] = None
        self._evaluated = False

    @property
    def enabled(self) -> bool:
        """False once :meth:`skip` has been called; ``evaluate`` becomes a no-op."""
        return self._skip_reason is None

    def skip(self, reason: str) -> None:
        """Opt out of the integrity check for this test at runtime.

        Must be called before ``execute_test()``. Raises ``RuntimeError`` if
        called after the verdict already ran -- a silent no-op there would
        look like it worked while leaving the real verdict unaffected.
        """
        if self._evaluated:
            raise RuntimeError(
                "media_integrity.skip() called after the integrity check "
                "already ran -- call skip() before execute_test()."
            )
        self._skip_reason = reason
        self._evaluated = True
        logger.info("Integrity check skipped: %s", reason)

    def evaluate(self, intent: IntegrityIntent, fail_on_error: bool = True) -> bool:
        """Run the RX/TX content comparison for this session.

        A test that requests the ``media_integrity`` fixture REQUIRES a real
        verdict unless it explicitly opted out via :meth:`skip` -- a missing
        source/output path is a hard failure, never a silent pass. Returns
        True when the content matches, was skipped, or not applicable.
        Returns False only when mismatched/unconfigured and
        ``fail_on_error`` is False. Raises ``AssertionError`` when
        mismatched/unconfigured and ``fail_on_error`` is True.
        """
        self._evaluated = True
        if not self.enabled:
            return True
        try:
            if not intent.src_url or not intent.out_url:
                self._fail(
                    "Integrity check required (test uses the media_integrity "
                    "fixture and did not opt out) but the app has no "
                    "input_file/output_file configured -- cannot verify "
                    "content integrity for this test.",
                    fail_on_error,
                )
            runner = self._build_runner(intent)
            if not runner.run():
                self._fail(
                    f"Integrity check failed content comparison for "
                    f"{intent.out_url} against {intent.src_url}.",
                    fail_on_error,
                )
            logger.info("Integrity check passed for %s", intent.out_url)
            return True
        except AssertionError:
            if fail_on_error:
                raise
            logger.info("Integrity check failed (fail_on_error=False); continuing")
            return False

    def _build_runner(self, intent: IntegrityIntent):
        out_name = os.path.basename(intent.out_url)
        out_path = os.path.dirname(intent.out_url)
        if intent.kind == "audio":
            channel = (intent.audio_channels or ["U02"])[0]
            return FileAudioIntegrityRunner(
                host=intent.host,
                test_repo_path=intent.test_repo_path,
                src_url=intent.src_url,
                out_name=out_name,
                sample_size=get_sample_size(intent.audio_format),
                sample_num=get_sample_number(intent.audio_sampling, intent.audio_ptime),
                channel_num=get_channel_number(channel),
                out_path=out_path,
                delete_file=False,
            )
        return FileVideoIntegrityRunner(
            host=intent.host,
            test_repo_path=intent.test_repo_path,
            src_url=intent.src_url,
            out_name=out_name,
            resolution=f"{intent.width}x{intent.height}",
            file_format=intent.file_format,
            out_path=out_path,
            delete_file=False,
        )

    def close(self, enforce_dispatch: bool = True) -> None:
        """Enforce the evaluated-exactly-once invariant.

        Called from the ``media_integrity`` fixture's teardown -- catches a
        test that requested the fixture but never called ``execute_test()``
        with it (or ``media_integrity.skip(...)``).

        *enforce_dispatch* is False when the test already failed before the
        verdict could run: the check legitimately never got its turn, and
        reporting it as "never dispatched" would bury the real failure under
        a second, misleading one.
        """
        if not self._evaluated and enforce_dispatch:
            log_fail(
                "Integrity check required (test uses the media_integrity "
                "fixture) but execute_test() never dispatched it -- ensure "
                "the test calls execute_test(integrity=media_integrity, ...) "
                "or media_integrity.skip(...)."
            )

    def _fail(self, msg: str, fail_on_error: bool) -> None:
        if fail_on_error:
            log_fail(msg)
        else:
            logger.info("Integrity check soft-fail (fail_on_error=False): %s", msg)
        raise AssertionError(msg)


class _NullIntegritySession:
    """No-op stand-in used when a test doesn't request the integrity fixture."""

    enabled = False

    def skip(self, reason: str) -> None:
        pass

    def evaluate(self, intent: IntegrityIntent, fail_on_error: bool = True) -> bool:
        return True

    def close(self, enforce_dispatch: bool = True) -> None:
        pass


if TYPE_CHECKING:
    # Static-only proof that both classes implement IntegrityCheck's full
    # surface -- a mypy/pyright run fails here (not just at a call site) the
    # moment either class drifts out of parity. No runtime cost/effect.
    _null_check: IntegrityCheck = _NullIntegritySession()
    _session_check: IntegrityCheck = IntegritySession()

NO_INTEGRITY = _NullIntegritySession()
