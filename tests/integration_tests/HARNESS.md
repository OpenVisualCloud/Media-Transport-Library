# KahawaiTest Architecture and Gated Refactoring Harness

`KahawaiTest` is the single integration test binary for the Media Transport
Library (MTL). Its run matrix, topologies, and CI groups live in
`tests/integration_tests/profiles.yaml`.

## Architecture Rules

1. **One binary, automatic mode selection.** `test_runtime.cpp` discovers all
   cases (`--gtest_list_tests`, `--list-json`), runs shared-context suites in
   one `mtl_init()` session, and dispatches `NoCtxTest` cases in isolated
   processes.
2. **Public APIs only.** Integration tests include only headers from
   `include/*.h`. They never include internal `lib/src/` headers.
3. **Shared RAII harness.** All shared suites use
   `tests/integration_tests/harness/`:
   - `harness/sessions.hpp` — `ScopedHandle`, `ScopedMtlStart`, `ScopedWorker`,
     `ScopedPluginFaults`, `ScopedLcore`, `ScopedSch`, `ScopedTasklet`, port
     fillers, and redundant stats checks.
   - `harness/memory.hpp` — `ScopedUdma`, `ScopedHpBuf`, `PageAlignedBuf`,
     `ScopedDmaMap`, `ScopedDmaMem`, `ScopedExtFrames`, and
     `udma_submit_and_wait`.
   - `harness/lifecycle.hpp` — type-safe RAII `verify_*` templates replacing
     legacy `#define` lifecycle macros.
   - `harness/mock_plugin.hpp` — `MockPluginPool<Traits>` for ST 20 converter
     and ST 22 encoder/decoder mock plugins.
4. **Fast, event-driven waits.** Tests wait on frame, packet, or stats events
   via `test_harness.hpp` instead of blind `sleep()` loops.
5. **Config contract.** Every compiled case must match at least one run in
   `profiles.yaml`, and every run filter must match at least one case (enforced
   by `tests/integration_tests/runner/test_nightly_workflow.py`).

## Mandatory Gated Refactoring Loop

Apply this gated pipeline to every subsystem slice defined from `profiles.yaml`:

1. **Analyst <-> Reviewer (Design Gate)**
   - The Analyst audits the current tests, the public `include/*.h` surface,
     and `profiles.yaml`, then writes the target architecture.
   - The adversarial Reviewer checks the target architecture. If the Reviewer
     raises any BLOCKER or MAJOR issue, loop back to the Analyst until the
     Reviewer approves.
2. **Planner <-> Reviewer (Plan Gate)**
   - The Planner turns the approved target architecture into an exact
     file-by-file change plan with disjoint parallel tasks and verification
     commands.
   - The Reviewer checks the plan. If the Reviewer disagrees, loop back to the
     Planner until the Reviewer approves.
3. **Parallel Developers <-> Reviewer (Code Gate)**
   - Spawn Developers in parallel on disjoint files to implement the approved
     plan.
   - The Reviewer audits the real `git diff`. If the Reviewer raises any
     BLOCKER or MAJOR finding, loop back to the Developer until approved.
4. **Tester (Verification Gate)**
   - Build `KahawaiTest`, check `clang-format --dry-run --Werror` and Python
     linters, run offline runner/workflow `pytest`, and run the affected
     `profiles.yaml` suites live on bare-metal E810-C hardware.
   - If any check or live test fails, loop back to the Developer, Reviewer, and
     Tester until all checks pass with zero failures.
