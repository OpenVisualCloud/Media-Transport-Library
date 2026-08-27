# GitHub Actions issue: prebuilt host dependencies

## Problem

MTL host setup currently has two phases:

1. [The build workflow](./workflows/build.yml) runs once on the `dpdk` fleet runner,
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

An ICE kernel module is tied to its target kernel. The bundle is a stash like
every other one -- `ice.ko` and `iavf.ko`, and no metadata beside them -- and its
cache key is:

- the ICE and IAVF versions, download identifiers and MTL patches, as one source
  hash; and
- the target kernel release and architecture.

The common validation-fleet kernel is a runner-provisioning invariant, so one
producer job serves the fleet. NIC model is not an ICE build input and must not
create separate producer jobs or cache entries.

The build must preserve both modules in a dedicated bundle directory before
removing the driver source tree. Validation reads the files it restored: it
rejects a bundle whose `modinfo -F vermagic` does not match the running kernel,
and one whose `ice.ko` has no `ice_vc_cfg_q_bw`, which is the Kahawai VF rate
limiter and the one thing a stock driver of the same kernel would pass without.

Comparing the cached module with `modinfo -n ice` is not enough, because the file
on disk can differ from the module already loaded in memory. Use `srcversion`:
the `.ko` carries one, the kernel exports the one it loaded, and both must equal
the cached file's before activation is skipped. No host state file is needed --
the kernel is the state.

ICE activation is a host-maintenance operation and must be implemented as one
idempotent Taskfile command. The processes that hold a NIC are already stopped by
the cleanup action every job runs first, so activation does not repeat that. When
the running modules differ it must:

1. remove the existing VFs of the `ice` PFs, and leave a card on another driver
  alone;
2. unload dependent modules such as `irdma`, then unload `ice`;
3. install and load the validated modules with the host's normal module tooling;
  and
4. verify that the loaded modules are now the cached ones.

It does not reload `irdma`: nothing in the suite uses RDMA, and the module returns
on the next boot. It must not recreate VFs: every consumer builds the VF state it
needs, and does it idempotently. If any step fails, validation fails. It must not
continue with an unknown driver or a partly configured NIC.

## Workflow authoring rules

GitHub Actions YAML is orchestration, not the implementation. In particular, do
not add multi-line `run: |` scripts to workflows or composite actions. Put each
operation behind a named [Taskfile](../Taskfile.yml) task backed by a focused,
human-runnable script when shell logic is required. The same command must work
locally and in CI, for example:

```sh
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
