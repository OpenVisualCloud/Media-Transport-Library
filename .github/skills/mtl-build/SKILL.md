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

## Format Code

`clang-format-14` is required (`sudo apt install clang-format-14`). **CI rejects
improperly formatted code.**

```bash
# Narrow change (PREFERRED): format ONLY the files you edited
clang-format-14 -i --style=file path/to/changed1.c path/to/changed2.h

# Whole-tree formatter — use only for a sweeping change
./format-coding.sh
```

- `format-coding.sh` reformats the **entire** repository. If any subtree has
  pre-existing format drift, it rewrites files you never touched and produces a
  noisy, hard-to-review diff.
- **After any format step run `git diff --name-only`** and keep only your own
  files' changes — `git checkout -- <unrelated files>` to revert the churn.
- Confirm your touched files are clang-clean before committing; that is all CI
  checks on the diff.

## Unit Tests (NOT built by `./build.sh`)

Unit gtests in `tests/unit/` need a separate build flag — `./build.sh` does not
build them.

```bash
meson setup build_unit -Denable_unit_tests=true   # one-time (or: meson configure build_unit -Denable_unit_tests=true)
ninja -C build_unit
./build_unit/tests/unit/UnitTest                   # NOTE the /unit/ path segment
./build_unit/tests/unit/UnitTest --gtest_filter='St20NewApiTxTest.*'   # one suite
```

- The binary is `build_unit/tests/unit/UnitTest` — **not** `build_unit/tests/UnitTest`.
- ASan is preloaded; any leak fails the whole run.
- Re-run the **full** `UnitTest` (not just your filter) before declaring green —
  ordering/teardown leaks only surface across the full suite.

## Verification Checklist

After building, verify:
1. Build completes without errors
2. Key artifacts exist:
   - `build/lib/libmtl.so` — shared library
   - `build/tests/KahawaiTest` — integration test binary
   - `build/app/RxTxApp` — reference application
3. Your touched files are clang-clean (see Format Code) and `git diff --name-only`
   shows no unrelated churn

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
| Build exits early / `jobserver` warnings | A broken ninja jobserver (often a user-local `~/.local/bin/ninja` ahead on `PATH`). Invoke the system binary explicitly: `/usr/bin/ninja -C build_unit`. For the lib `build/` dir, prefer MCP `build_mtl` / `mtl_clean_rebuild`. |
| Phantom test crash / segfault that does **not** reproduce on a clean tree | Stale or root-owned `build*/.ninja_deps` from a prior privileged build. Clean-rebuild (`rm -rf build_unit && meson setup build_unit -Denable_unit_tests=true`) and re-verify before trusting any reported crash. |
