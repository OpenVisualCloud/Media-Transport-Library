# GitHub Actions issue: prebuilt host dependencies

## Problem

MTL host setup currently has two phases:

1. [The build workflow](./workflows/build.yml) runs once on the `e835` fleet runner,
  computes source hashes, builds the required components, and stores them in
  the GitHub Actions cache.
2. The `validate-host` action runs on each bare-metal runner. It should restore
  those components and verify that the host is ready for testing.

The boundary is currently incomplete. Host validation still builds and installs
the ICE driver and SVT-JPEG-XS, while the gtest workflow performs another ICE
driver check. This makes validation slow, gives test jobs permission to mutate
the host, and can produce different binaries on different runners.

See [the CI/CD architecture proposal](../doc/cicd_setup_proposition.md) and the
[architecture diagram](./ci_arch.svg) for the wider pipeline.

## Proposed architecture

Build expensive dependencies once and make host jobs consumers of immutable,
content-addressed outputs:

1. Build SVT-JPEG-XS and its MTL bridge plugin in `build.yml`, installed under
  `.local_install/jpegxs` without writing to `/usr/local`.
2. Build one patched ICE module in `build.yml`. The dedicated `e810`, `e830`,
  and `e835` hardware validation hosts are maintained on the same kernel ABI,
  so every validation job consumes the same artifact.
3. Store each successful output under a deterministic SHA-256 cache key.
4. Make `validate-host` restore, validate, and, only when required, activate the
  outputs.
5. Remove all driver build, install, and reload behavior from gtest steps. Tests
  must only test.

### JPEG XS artifact

The JPEG XS output is a bundle, not only one shared object. It must contain:

- the SVT-JPEG-XS runtime libraries;
- its `pkg-config` metadata;
- the MTL JPEG XS bridge plugin; and
- any required runtime data or symlinks.

Its cache key must include the pinned `SVT_JPEG_XS_VER`, MTL hash, architecture,
toolchain/build options, and the sources used by the bridge plugin. FFmpeg's key
must also include the JPEG XS hash whenever FFmpeg is built with JPEG XS support.

Host validation must verify the expected libraries, plugin, and `pkg-config`
entry before exporting the local library and plugin paths. It must never fall
back to compiling or installing JPEG XS on the test host.

### ICE artifact

An ICE kernel module is tied to its target kernel. The cache key must include:

- ICE version, Intel download identifier, and all MTL ICE patches;
- target kernel release and architecture;
- relevant kernel build configuration or headers; and
- compiler/toolchain identity when it affects module compatibility.

The common validation-fleet kernel is a runner-provisioning invariant. The
single producer runs on one fleet runner so an unrelated build runner's kernel
cannot select the artifact ABI. NIC model is not an ICE build input and must
not create separate producer jobs or cache entries. Validation still checks the
module metadata against the running kernel and fails clearly if a runner has
drifted from the fleet kernel.

The build must preserve `ice.ko` in a dedicated artifact directory before
removing the driver source tree. Validation must reject an artifact when
`modinfo -F vermagic` does not match the running kernel. Secure Boot and module
signing requirements must also be checked explicitly.

Use SHA-256 rather than MD5 for artifact identity. Comparing the cached module
with `modinfo -n ice` is not enough because the file on disk can differ from the
module already loaded in memory. After a successful activation, store the
artifact SHA-256 in a root-owned host state file and require both that state and
the expected loaded Kahawai version to match before skipping activation.

ICE activation is a host-maintenance operation and must be implemented as one
idempotent Taskfile command. When the hash differs it must:

1. acquire a per-host lock to prevent concurrent jobs from changing the NIC;
2. stop processes using the NIC and remove existing VFs;
3. unload dependent modules such as `irdma`, then unload `ice`;
4. install/load the validated module using the host's normal module tooling;
5. verify the loaded version and capabilities;
6. reload optional `irdma` on a best-effort basis without rolling back valid
  ICE when the reload fails;
7. recreate the required VFs; and
8. update the host state file only after all checks succeed.

If any step fails, validation fails. It must not continue with an unknown driver
or partially configured NIC.

## Workflow authoring rules

GitHub Actions YAML is orchestration, not the implementation. In particular, do
not add multi-line `run: |` scripts to workflows or composite actions. Put each
operation behind a named [Taskfile](../Taskfile.yml) task backed by a focused,
human-runnable script when shell logic is required. The same command must work
locally and in CI, for example:

```sh
task ci:build-ice
task ci:validate-ice
task ci:build-jpegxs
task ci:validate-jpegxs
```

This keeps logs grouped by operation, makes failures reproducible without a
runner, and avoids maintaining separate local and CI implementations.

Additional rules adopted for this work:

- pin third-party actions to immutable commit SHAs;
- grant the smallest job-level `permissions` needed;
- set explicit job timeouts and concurrency controls for each physical host;
- keep build, validation, and test responsibilities separate;
- use caches for reusable content-addressed dependencies and artifacts for run
 outputs such as logs and reports;
- restore caches before doing expensive work, validate their contents, and save
 them only after a successful build;
- make dependencies and cache-key inputs explicit;
- fail early with actionable compatibility checks;
- keep secrets out of command lines, logs, caches, and artifacts; and
- run the same Taskfile entry points in the local CI harness before merging.

These practices are informed by DevOps Directive's
[Complete GitHub Actions Course - From BEGINNER to PRO](https://www.youtube.com/watch?v=Xwpi0ITkL3U&t=6767s)
and adapted to this repository's self-hosted, hardware-backed runners.

## Acceptance criteria

- `build.yml` produces validated JPEG XS and kernel-compatible ICE outputs.
- A cache hit never triggers compilation on a bare-metal test runner.
- A malformed or incompatible cache entry fails host validation immediately.
- `validate-host` changes the loaded ICE module only when the validated artifact
 differs from the activated host state.
- VFs are available again after any driver activation.
- Gtest and pytest workflows contain no ICE/JPEG XS build or install step.
- Workflow and composite-action YAML call named Taskfile tasks instead of
 embedding multi-line shell programs.
- Every new task can be invoked locally and has a focused validation path.
