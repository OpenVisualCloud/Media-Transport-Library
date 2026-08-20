---
name: mtl-build
description: 'Build MTL library, format code, and verify compilation. Use when asked to build, verify changes, check formatting, or prepare code for commit.'
---

# Build & Verify MTL

## Build Modes

Run from the repository root:

| Command | Mode | Use when |
|---------|------|----------|
| `./build.sh` | Release | Default. Optimized build for testing and deployment |
| `./build.sh debug` | Debug + ASAN | Debugging with AddressSanitizer (`-O0 -g`). Slower but catches memory bugs |
| `./build.sh debugonly` | Debug only | Debug symbols without ASAN. Faster than `debug` for general development |
| `./build.sh unit` | Unit gtest | Configures `build_unit/` with `-Denable_unit_tests=true`, builds it, and runs `tests/unit/UnitTest`. Combine with `debug` (e.g. `./build.sh debug unit`) to run under ASan via `LD_PRELOAD` |

## Format Code

```bash
./format-coding.sh
```

- `./format-coding.sh --check` previews changes (runs the fixers on a clean
  tree, prints the full diff, then restores it; exits 1 if anything would
  change). Use it to inspect a formatting change's blast radius. It refuses to
  run on a dirty tree — commit or stash uncommitted work first.
- `./checkpatch.sh` is the same rule set in verify mode.
  `./checkpatch.sh --staged` checks only staged files — what the git hook runs.
- Requires only `pre-commit` (`./checkpatch.sh --bootstrap`, or the distribution
  package it names on a PEP 668 host such as Fedora, Arch or Debian 12+). It
  installs the pinned clang-format, shfmt, shellcheck, markdownlint, yamllint,
  actionlint and gitleaks itself — do **not** `apt install clang-format-22`, and
  do not rely on whatever version happens to be on `PATH`.
- **CI runs the identical hook list** — always run before committing
- Covers C/C++, Python (`isort`, `black`, `flake8`, `ruff`), shell, Markdown
  (syntax and prose), YAML, GitHub Actions workflows, HTML and secrets
- Rules live in `.pre-commit-config.yaml` and `.github/linters/` only. See
  [doc/coding_standard.md](../../../doc/coding_standard.md)

## Verification Checklist

After building, verify:
1. Build completes without errors
2. Key artifacts exist:
   - `build/lib/libmtl.so` — shared library
   - `build/tests/KahawaiTest` — integration test binary
   - `build/app/RxTxApp` — reference application
3. Run `./checkpatch.sh` and check that it exits clean

## Common Build Errors

| Error | Fix |
|-------|-----|
| Missing `meson` / `ninja` | `sudo apt install meson ninja-build` |
| Missing `libnuma-dev` | `sudo apt install libnuma-dev` |
| Missing `libjson-c-dev` | `sudo apt install libjson-c-dev` |
| Missing `libpcap-dev` | `sudo apt install libpcap-dev` |
| Missing `libgtest-dev` | `sudo apt install libgtest-dev` |
| Missing `libssl-dev` | `sudo apt install libssl-dev` |
| Missing `systemtap-sdt-dev` | `sudo apt install systemtap-sdt-dev` (for USDT probes) |
| DPDK not found / wrong version | Build DPDK first: see [build docs](../../doc/build.md) |
| Stale build directory | `rm -rf build && ./build.sh` or use MCP tool `mtl_clean_rebuild` |
| Permission errors in build/ | `sudo rm -rf build && ./build.sh` |
