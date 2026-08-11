---
name: mtl-cicd
description: 'Design, implement, debug, and validate MTL GitHub Actions CI/CD, Taskfile tasks, local CI jobs, caches, artifacts, self-hosted runners, ICE modules, and JPEG XS dependencies. Use for workflow changes, validate-host changes, cache-key changes, bare-metal CI, or requests to test locally and push CI updates.'
---

# MTL CI/CD

## Design contract

- GitHub Actions YAML orchestrates jobs; it does not implement operations.
- Do not add multi-line `run: |` programs to workflows or composite actions.
- Put each operation behind a named root `Taskfile.yml` task. Put substantial
  shell logic in a focused, human-runnable script called by that task.
- Local CI and GitHub Actions must invoke the same Taskfile entry points.
- Keep build, host validation, test execution, cleanup, and report collection
  separate.
- Pin third-party actions to immutable commit SHAs, minimize job permissions,
  set timeouts, and serialize operations that mutate a physical host.

Read the current contracts before editing:

- `.github/github_actions_issue.md`
- `doc/cicd_setup_proposition.md`
- `.github/instructions/mtl-system-setup.instructions.md`
- `.github/ci-local/README.md`

## Artifact and cache rules

- Use deterministic SHA-256 source hashes and explicit dependency waterfalls.
- Include architecture, toolchain, build options, and external pinned versions
  where they affect output compatibility.
- Restore and structurally validate a cache before treating it as a hit.
- Use `actions/cache/restore` and save only after successful validation; never
  allow a failed partial build to poison an immutable cache key.
- Use caches for reusable dependencies. Use workflow artifacts for logs,
  reports, and other run-specific outputs.
- Keep install trees under `.local_install`; test hosts must not compile a
  missing cached dependency as a fallback.

## ICE module rules

- Build one ICE artifact on the `e835` validation-fleet runner in `build.yml`;
  every NIC validation job restores and activates that artifact. Do not build
  ICE on `dpdk` or `e810`: both labels can select the generic builder whose
  kernel can drift from the fleet.
  Do not create per-NIC ICE producer jobs or include the NIC model in its key.
- An ICE module artifact is specific to its source/patch hash, kernel release,
  architecture, relevant kernel configuration/headers, and compiler ABI.
- Validate `modinfo -F vermagic`, expected Kahawai version, architecture,
  Secure Boot, and module-signing compatibility before touching the NIC.
- Compare the desired artifact with a root-owned activation stamp and verify
  the loaded module. Comparing only the module file on disk is insufficient.
- Activation must be idempotent and protected by a per-host lock. Stop NIC
  users, remove VFs, unload dependent modules and ICE, load the validated
  module, verify it, recreate VFs, then update the stamp.
- Gtest and pytest workflows never compile, install, or reload ICE.

## JPEG XS rules

- Treat JPEG XS as a bundle: SVT-JPEG-XS runtime, `pkg-config` metadata, MTL
  bridge plugin, symlinks, and required runtime files.
- Its hash includes `SVT_JPEG_XS_VER`, MTL hash, bridge sources, architecture,
  toolchain, and build options.
- FFmpeg's hash includes the JPEG XS hash when JPEG XS support is enabled.
- Consumers use local library, package, and plugin paths; do not install the
  bundle into `/usr/local` on test hosts.

## Implementation workflow

1. Inspect `git status`, preserve unrelated and user-authored changes, and
   identify the exact workflow/action/task/script path controlling behavior.
2. Add or update the smallest Taskfile task and focused script first.
3. Add a narrow local test that proves cache identity, artifact validation,
   and failure behavior without requiring a NIC where possible.
4. Make workflows/composite actions call the task with one-line commands.
5. Update `.github/ci-local/` so it executes the same task and models cache
   restore/save behavior rather than duplicating production logic.
6. Run focused tests, `task ci:build`, each affected `task ci:validate-host`
   NIC variant, then `task ci:test-pr` when feasible.
7. Run `git diff --check` and inspect the final diff for inline scripts,
   unpinned actions, excessive permissions, and unrelated changes.
8. Push only when the user explicitly requested it, all available local gates
   are green, and the pushed commit contains only the intended saved changes.

## Local CI MCP contract

- `.github/mcp/mtl_ci_mcp_server.py` is the agent-facing adapter for the
  human-runnable scripts under `.github/ci-local/`; every documented MCP tool
  must exist and invoke the same script or Taskfile entry point documented in
  `.github/ci-local/README.md`.
- Keep MCP result formatting on the shared
  `.github/mcp/mtl_setup_common.py::_summarize_output(name, output, rc=...)`
  contract. Exercise each wrapper far enough to format its result; syntax-only
  checks do not catch unassigned result variables or bad helper arity.
- `ci_test_pr` must summarize `.github/ci-local/test-pr-locally.sh`, and
  `ci_run_task` must return the shared summary for `task`. These paths are the
  first focused checks when the local MCP wrappers change.

## Production CI MCP contract

- Use `ci_pr_checks` as the first check after pushing. It queries GitHub through
  the authenticated `gh` CLI and returns a bounded table, failed checks first.
- Use `ci_pr_failures` only when checks fail. Query check-run annotations first;
  fetch failed-job logs only as a fallback, strip runner prefixes, and return a
  small set of actionable error lines instead of complete workflow logs.
- Treat only failure-level check annotations as diagnostics. Warning and notice
  annotations must not suppress the failed-log fallback.
- Cap check rows, failed-check sections, annotations, and failure excerpts;
  report omitted counts at every truncated level. Prefer explicit linter
  diagnostic blocks and affected-file lines over trailing runner warnings.
- Default the repository from `git remote get-url origin`; allow an explicit
  `owner/repo` override. Validate repository and PR inputs before invoking `gh`.
- Production tools are read-only. They may inspect or wait for checks but never
  push, rerun, comment, merge, or expose authentication details.
- Never include raw `gh` stderr in MCP results. Return the command class and exit
  status; authentication diagnostics can contain credentials.
- Verify production wrappers with stubbed `gh` output before deployment, then
  restart the MCP server and call the deployed tool against the target PR.
- Run production Black, isort, and Ruff against changed Python MCP files before
  pushing; syntax compilation alone is insufficient.

## Failure policy

- Fail early with an actionable compatibility error. Never continue with an
  unknown driver, malformed cache, or partially configured NIC.
- Do not hide failures behind `|| true` unless the operation is explicitly
  best-effort and the reason is documented.
- Never print secrets or put them in command lines, caches, or artifacts.
- If hardware-only validation is unavailable locally, report that gate clearly
  and do not represent container simulation as physical-host validation.

## eBPF/XDP install contract

- Install bundled libbpf into `/usr/local/lib/$(cc -dumpmachine)`, not libbpf's
  `/usr/local/lib64` default. Ubuntu's `pkg-config` does not search the latter,
  so CI can install `libbpf.pc` successfully and still fail `--require-xdp`.
- Run `ldconfig` after installing xdp-tools and libbpf, then verify the pinned
  versions through the same `ebpf:check REQUIRE_XDP=1` path production uses.
- The manager's clang `-target bpf` compile must include the host compiler's
  multiarch directory (`/usr/include/$(cc -dumpmachine)`), because BPF-target
  clang does not discover Ubuntu's `asm/types.h` path automatically.
- A generic failure annotation such as "Process completed with exit code" is
  not actionable. Keep it in the result, but also inspect the bounded failed
  log so the underlying compiler or linter error is returned.