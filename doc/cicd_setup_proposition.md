# Proposed setup of the architecture

To confirm the and run the project automatic validation you would need github
self hosted runners with tags e810 e830 e835 that will run validation.

here is propsed way you set it up in github.

```mermaid
flowchart TD
    PR_TRIGGER([PR TRIGGER])
    LINTER[LINTER]
    BUILD[BUILD]
    SMOKE_FILTER{SMOKE\nPATH\nFILTER}
    GTEST_SMOKE[GTEST / SMOKE]
    CHECK_MD5_1([CHECKS THE\nSOURCE MD5SUM])
    UPLOAD_STASH([UPLOADS TO\nGITHUB STASH])
    GITHUB_STASH[(GITHUB STASH\n\nMTL ARTIFACT\nTEST ARTIFACT\nFFMPEG ARTIFACT\nGSTREAMER ARTIFACT\nDPDK ARTIFACT)]
    NIGHTLY[NIGHTLY / GTEST NIGHTLY]

    %% PR Pipeline - Left side
    BM_HOST_1[[BARE METAL HOST]]
    BM_HOST_2[[BARE METAL HOST]]
    CHECK_MD5_2([CHECKS THE\nSOURCE MD5SUM])
    CHECK_MD5_3([CHECKS THE\nSOURCE MD5SUM])
    DL_ART_1([DOWNLOADS\nARTIFACTS])
    DL_ART_2([DOWNLOADS\nARTIFACTS])
    VALIDATE_1([VALIDATE BUILD\n\nSETS THE ARTIFACTS\nIN CORRECT PLACES\nCHECK / BUILDS\nICE DRIVER IF NEEDED])
    VALIDATE_2([VALIDATE BUILD\n\nSETS THE ARTIFACTS\nIN CORRECT PLACES\nCHECK / BUILDS\nICE DRIVER IF NEEDED])
    SHADOW_HOST_1[[SHADOW HOST]]
    TESTS_1([TESTS])
    TESTS_2([TESTS])

    %% Nightly Pipeline - Right side
    BM_HOST_3[[BARE METAL HOST]]
    BM_HOST_4[[BARE METAL HOST]]
    CHECK_MD5_4([CHECKS THE\nSOURCE MD5SUM])
    CHECK_MD5_5([CHECKS THE\nSOURCE MD5SUM])
    DL_ART_3([DOWNLOADS\nARTIFACTS])
    DL_ART_4([DOWNLOADS\nARTIFACTS])
    VALIDATE_3([VALIDATE BUILD\n\nSETS THE ARTIFACTS\nIN CORRECT PLACES\nCHECK / BUILDS\nICE DRIVER IF NEEDED])
    VALIDATE_4([VALIDATE BUILD\n\nSETS THE ARTIFACTS\nIN CORRECT PLACES\nCHECK / BUILDS\nICE DRIVER IF NEEDED])
    SHADOW_HOST_2[[SHADOW HOST]]
    TESTS_3([TESTS])
    TESTS_4([TESTS])
    RAPORT[RAPORT]
    CREATE_REL([CREATE REL\nCANDIDATE])

    %% PR Trigger connections
    PR_TRIGGER --> LINTER
    PR_TRIGGER --> BUILD
    PR_TRIGGER --> GTEST_SMOKE

    %% Main pipeline flow
    LINTER -->|WAITS| BUILD
    SMOKE_FILTER -->|WAITS| BUILD
    BUILD --> CHECK_MD5_1
    BUILD --> SMOKE_FILTER
    SMOKE_FILTER --> GTEST_SMOKE

    %% Build artifact upload
    CHECK_MD5_1 --> UPLOAD_STASH
    UPLOAD_STASH -->|UPLOAD| GITHUB_STASH

    %% GTEST/SMOKE to Bare Metal Hosts
    GTEST_SMOKE --> BM_HOST_1
    GTEST_SMOKE --> BM_HOST_2

    %% Left branch - BM_HOST_1
    BM_HOST_1 --> CHECK_MD5_2
    CHECK_MD5_2 --> DL_ART_1
    GITHUB_STASH -.->|UPLOAD| DL_ART_1
    DL_ART_1 --> VALIDATE_1
    VALIDATE_1 -->|IF NEEDS AND EXISTS| SHADOW_HOST_1
    VALIDATE_1 --> TESTS_1
    SHADOW_HOST_1 <--> TESTS_1

    %% Right branch - BM_HOST_2
    BM_HOST_2 --> CHECK_MD5_3
    CHECK_MD5_3 --> DL_ART_2
    GITHUB_STASH -.->|UPLOAD| DL_ART_2
    DL_ART_2 --> VALIDATE_2
    VALIDATE_2 --> TESTS_2

    %% Nightly pipeline
    NIGHTLY --> BM_HOST_3
    NIGHTLY --> BM_HOST_4

    %% Nightly Left branch - BM_HOST_3
    BM_HOST_3 --> CHECK_MD5_4
    CHECK_MD5_4 --> DL_ART_3
    GITHUB_STASH -.->|UPLOAD| DL_ART_3
    DL_ART_3 --> VALIDATE_3
    VALIDATE_3 -->|SETS| SHADOW_HOST_2
    VALIDATE_3 --> TESTS_3

    %% Nightly Right branch - BM_HOST_4
    BM_HOST_4 --> CHECK_MD5_5
    CHECK_MD5_5 --> DL_ART_4
    GITHUB_STASH -.->|UPLOAD| DL_ART_4
    DL_ART_4 --> VALIDATE_4
    VALIDATE_4 --> TESTS_4

    %% Report and Release
    TESTS_3 -->|WAITS| RAPORT
    TESTS_4 -->|WAITS| RAPORT
    RAPORT --> CREATE_REL
```

## The stash, as implemented today

`BUILD` and the `GITHUB STASH` above are `.github/workflows/build.yml`. The
artifact store is `actions/cache`, keyed on a checksum of the sources that
produce each artifact, so a run only pays for the components a pull request
actually touched.

| stage in the diagram      | implementation                                      |
| ------------------------- | --------------------------------------------------- |
| `LINTER` / `WAITS`        | `wait-for-linter` job                               |
| `CHECKS THE SOURCE MD5SUM` | `checksums` job, `script/hash_sources.sh`          |
| `GITHUB STASH`            | `actions/cache`, keys `stash-<component>-<checksum>` |
| `BUILD`                   | `build` job on the self-hosted `dpdk` runner        |

The checksums waterfall, so that rebuilding a component invalidates everything
downstream of it:

```mermaid
flowchart LR
    DPDK[dpdk sources] --> H_DPDK([hash: dpdk])
    MTL[mtl sources] --> H_MTL([hash: mtl])
    H_DPDK --> H_MTL
    H_MTL --> H_FFMPEG([hash: ffmpeg])
    H_MTL --> H_GST([hash: gstreamer])
    H_MTL --> H_PLUGINS([hash: plugins])
```

The path lists that feed each hash live in `script/hash_sources_*.env`.

### The failure mode this design has

`actions/cache` saves in a post step that runs whether the job passed or not.
A run that dies part-way through installing a component still stores that
half-written tree under the key derived from its sources. Every later run with
the same sources restores it, reads `cache-hit == true`, skips the build, and
fails on whatever the tree is missing. The source checksum is unchanged, so
the poisoned entry cannot expire on its own — it has to be evicted by hand or
outlived.

Observed as: `MTL=HIT` in the cache notice, followed by
`ERROR: mtl >= 22.12.0 not found using pkg-config`, on a branch that had not
touched a single file feeding the `mtl` hash.

Two rules close it, both already implemented in the local harness below:

1. a restored tree counts as a hit only if it is structurally usable — the
   artifact consumers resolve is present (`libdpdk.pc`, `mtl.pc`,
   `libavcodec.pc`, a plugin `.so`) — otherwise it is treated as a miss;
2. the cache is written only when the job succeeded, which for
   `actions/cache` means splitting it into `actions/cache/restore` plus an
   explicit save step gated on `if: success()`.

## Running the pipeline locally

`.github/ci-local/` runs these jobs on a developer machine, in a container that
simulates the runner, against a persistent `.local_install` so that only what
changed is rebuilt.

```sh
.github/ci-local/run-job.sh build
```

It reuses the workflow's own moving parts rather than restating them — the same
`script/hash_sources.sh` for keys, the same
`.github/scripts/setup_environment.sh` for the build — so a failure reproduced
locally is the CI failure, and a fix proven locally is a fix. See
[.github/ci-local/README.md](../.github/ci-local/README.md) for the mapping
between each GitHub-provided step and its local stand-in.
