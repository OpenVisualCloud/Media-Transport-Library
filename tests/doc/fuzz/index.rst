==========
Fuzz Tests
==========

The ``tests/fuzz`` directory holds libFuzzer harnesses. Each harness sends each
input to one parser of the library: the ST 2110-20, -22, -30 and -40 RX packet
paths, the ST 2110-40 helper functions, and the RTCP NACK parser of the TX side.
Sphinx makes the `Targets`_ list from ``tests/fuzz/meson.build`` and from the
file comment of each harness, so the list and the build always agree.

Build Requirements
==================

* Clang with ``-fsanitize=fuzzer`` (libFuzzer). gcc has no libFuzzer, so the
  configure step of ``tests/fuzz/meson.build`` stops with gcc.
* DPDK, installed as for the library build.
* Linux. The RX harnesses start an EAL with ``--no-huge``, so they need no
  hugepages, no NIC and no root.

Clang 14 (Ubuntu 22.04) and clang 21 build the harnesses. Clang 18 (Ubuntu 24.04)
rejects 50 atomic operations in ``lib/`` (T-115).

Build
=====

Build the harnesses with AddressSanitizer in a separate build directory::

    CC=clang CXX=clang++ meson setup build_fuzz -Denable_fuzzing=true \
      -Db_sanitize=address -Db_lundef=false \
      -Dc_args=-Wno-error=unused-but-set-variable
    ninja -C build_fuzz

With clang, use ``-Db_sanitize=address``, not ``-Denable_asan=true``:
``lib/meson.build`` looks for the gcc ``libasan``, and clang does not supply it.
The ``-Wno-error`` flag stops the clang warning about the unused ``sent`` counter
in ``st_video_transmitter.c`` from failing the build.

``CC=clang CXX=clang++ ./build.sh release enable_fuzzing`` (or
``MTL_BUILD_ENABLE_FUZZING=true``) also adds the harnesses to ``build/``, with no
sanitizer. The base build CI job uses this form to check that the harnesses
compile.

Symbol Preemption
-----------------

Five of the six harnesses ``#include`` a production ``.c`` file, so that its
static functions become visible. Such a harness defines the same non-static
symbols as ``libmtl`` and needs no linker flag. ``libmtl`` is a shared library,
and a definition in the executable preempts it. Preemption is global, so a call
from inside ``libmtl`` also goes to the harness copy. This is true only while
the build sets no ``-fvisibility=hidden``, ``-Bsymbolic`` or
``-fno-semantic-interposition``. These flags bind the call locally, and the
harness then silently loses the coverage. Each target links one harness source
only, so two harnesses cannot collide.

Targets
=======

.. fuzz-targets::

Run a Target
============

Each target is a standalone libFuzzer executable. Give it a writable corpus
directory::

    mkdir -p corpus/st30_rx_frame_fuzz
    ./build_fuzz/tests/fuzz/st30_rx_frame_fuzz -max_total_time=60 corpus/st30_rx_frame_fuzz

The RX harnesses start the EAL with
``--no-huge --no-shconf -c1 -n1 --no-pci --vdev=net_null0``. Because of ``-c1``,
each of them runs on CPU 0, so two harnesses that run at the same time share
one CPU. Run them one after the other.

When a target finds a fault, it writes the input to a ``crash-``, ``leak-`` or
``timeout-`` file. To run the target again on that input only, give the file as
the argument::

    ./build_fuzz/tests/fuzz/st30_rx_frame_fuzz crash-<sha1>

Logs
----

The RX harnesses set the MTL and the DPDK log level to ``DEBUG``, and send each
log line to ``stderr``. The log shows the ``MTL:`` lines for the payload type
and SSRC mismatches, the redundant packets and the enqueue failures.
``mt_rtcp_tx_parse_fuzz`` sets the MTL log level to ``CRIT``.
``st40_ancillary_helpers_fuzz`` writes no log.

CI
==

The ``fuzz-tests`` job of ``.github/workflows/fuzz_tests.yml`` runs on a
GitHub-hosted ``ubuntu-22.04`` runner. It builds the harnesses with clang and
AddressSanitizer, and runs each target for ``FUZZ_TIME`` seconds, one after the
other. The job fails when a target exits with a code that is not zero, or
writes a ``crash-``, ``leak-`` or ``timeout-`` file.

The workflow runs in three cases:

* A pull request to ``main`` or to a ``maint-*`` branch, with 60 s for each
  target. The job runs only when the pull request changes a path of the
  ``fuzz_tests`` filter in ``.github/path_filters.yml``: ``lib/``,
  ``include/``, ``tests/fuzz/``, the DPDK inputs, the root meson files or the
  workflow files. Each harness links ``libmtl``, so each file of ``lib/`` can
  change a target. For a different change, the job does not run.
* A schedule, each night on ``main``, with 600 s for each target.
* A manual run (``workflow_dispatch``), with 600 s for each target.

The ``fuzz-tests-result`` job always runs, also when the filter skips the fuzz
job. It fails only when the fuzz job fails or is cancelled, so it is the check
to require in branch protection.

The corpus stays in the Actions cache from one run to the next. A pull request
can read the corpus of ``main``.

The ``fuzz-report`` artifact holds:

* ``summary.md``: one line for each target, with the executions, the coverage
  and the finding. The job summary shows the same table.
* ``logs/``: the full libFuzzer output of each target.
* ``artifacts/``: the inputs that caused a fault.
* ``repro/``: the output of the target on each such input, with the sanitizer
  stack trace.

The same script runs on a development host::

    task ci:fuzz -- build
    FUZZ_TIME=60 task ci:fuzz -- run

``FUZZ_BUILD_DIR`` (default ``build_fuzz``) and ``FUZZ_REPORT_DIR`` (default
``fuzz-report``) change the two directories.

Pytest Integration
==================

``tests/acceptance/fuzzing/test_fuzzing.py`` runs each target for
``MTL_FUZZ_TEST_RUNS`` inputs (default 500000), and sends the libFuzzer and MTL
output to ``tests/acceptance/logs/latest/pytest.log``. ``MTL_FUZZ_BUILD_DIR``
(default ``build``) selects the build directory::

    MTL_FUZZ_BUILD_DIR=build_fuzz pytest tests/acceptance/fuzzing/test_fuzzing.py

.. automodule:: fuzzing.test_fuzzing
   :members: test_fuzz_target_full_run
