---
description: "Use to one-shot prepare a single Linux host to run the MTL Python pytest framework under tests/validation/. Runs .github/scripts/setup_validation.sh which handles apt + DPDK + ICE + MTL + RxTxApp builds, hugepages, NFS at /mnt/media, passwordless SSH to root@127.0.0.1, venv, and configs/{topology,test}_config.yaml. Optionally builds the in-repo FFmpeg/GStreamer plugins. Always idempotent. DOES NOT run pytest — returns the configs and a recommended selector so the parent agent (.github/instructions/mtl-validation-tests.instructions.md) can run the tests."
name: "MTL Validation Setup"
tools: [execute, read, search, edit, todo]
user-invocable: true
---

# MTL Validation Setup

Prepare a single Linux host for `tests/validation/tests/single/` pytest. Hand back to the
parent instruction ([mtl-validation-tests](../instructions/mtl-validation-tests.instructions.md))
to actually run pytest. **Never run pytest beyond `--collect-only -q`.**

## Principles

- **Run `.github/scripts/setup_validation.sh`. Don't reimplement it.** Read its 30-line header
  for stages, knobs, and inputs. Idempotent — re-running on a prepared host is ~10 s.
- **Don't fight the framework.** Never edit `tests/validation/conftest.py`, `common/`, or
  `mtl_engine/`. Test failures = env or YAML wrong, not the test.
- **Preserve user state.** Existing configs are kept; delete to regenerate.

## Workflow

### 1. Probe

```bash
lspci -nn | grep -iE 'ethernet.*intel'                       # NIC vendor:device + BDF
findmnt -no SOURCE /mnt/media 2>/dev/null || echo NOT_MOUNTED # NFS state (skip re-prompting if mounted)
mount | grep nfs ; grep -E 'nfs|media' /etc/fstab 2>/dev/null # NFS hints
ls ~/.ssh/id_{ed25519,rsa,ecdsa} 2>/dev/null                 # SSH keys
# Live-vs-installed ice driver: stock kernel ice causes RL pacing to segfault.
# The script reloads automatically when patched ice is installed but stock is live.
modinfo ice    | awk '/^(filename|version):/'
modinfo -n ice                                                # what's actually loadable
ls tests/validation/configs/{topology,test}_config.yaml \
   build/manager/MtlManager tests/tools/RxTxApp/build/RxTxApp \
   tests/validation/venv/bin/python3 2>&1                     # already prepared?
CHECK_ONLY=1 bash .github/scripts/setup_validation.sh         # one-shot probe of every stage
```

### 2. MUST-ASK before running

**Always ask the human for `NFS_SOURCE`** unless `findmnt -no SOURCE /mnt/media`
already returns a value on this host (in which case reuse it). Without media,
almost no `tests/single/` test will run (`st20p`, `st22p`, `st30p`, `st40p`,
`st41`, `ffmpeg`, `gstreamer`, `kernel_socket`, `ptp`, `rss_mode`,
`virtio_user` — all require it). The script hard-fails fast at STAGE_NFS when
`NFS_SOURCE` is empty and `/mnt/media` is empty — that is intentional, do not
skip the stage to work around it.

Never assume a default NFS server. `10.123.232.121:/mnt/NFS/mtl_assets/media`
is a known **lab** default — mention it as a suggestion when prompting, never
as an assumed value. Every host has a different storage server.

Also ask only when not auto-detectable:
- Multiple candidate PFs → ask which BDF.
- FFmpeg / GStreamer plugin builds → opt-in only when those test categories were named.
- EBU compliance creds → only if compliance verdict was requested.

### 3. Run the script

```bash
NFS_SOURCE=<host>:<export> bash .github/scripts/setup_validation.sh
# add STAGE_FFMPEG_PLUGIN=1 STAGE_GST_PLUGIN=1 for those test categories
# set any STAGE_*=0 to skip a step (NFS=0 only with explicit user consent)
# NFS_PERSIST=1 to also write /etc/fstab
# VERBOSE=1 to stream wrapped-command output live (default: captured, shown only on failure)
# CHECK_ONLY=1 to probe every stage and report "would install" without modifying the host
```

Full run output is also tee'd to `/tmp/setup_validation-<UTC>.log`; the path is
printed in the script's banner and final summary.

**Expected duration.** Cold install ≈ 7–10 min (DPDK 2–4 min + MTL 1–3 min +
ICE 30–90 s + apt 30–60 s + venv 20–40 s). Warm re-run < 5 s. Stream the
output; do **not** time out at 60 s. Banner printed at script start lists the
stages and time budget.

### 4. Sanity collect-only

```bash
cd tests/validation && sudo -E ./venv/bin/python3 -m pytest \
  --topology_config=configs/topology_config.yaml \
  --test_config=configs/test_config.yaml \
  <selector> --collect-only -q
```

If collection fails, see the failure table in the parent instruction, apply the fix, re-run §3.

## When the script doesn't cover it

If a setup symptom isn't fixed by re-running `setup_validation.sh`:

1. Read `tests/validation/logs/latest/*.log` (sudo) and `/tmp/setup_validation-*.log`
   to identify the gap.
2. Prefer **tightening an existing stage's probe or install path** over inventing
   a new stage — e.g. add a version check, an extra apt package, a missing
   `modprobe`. (This is how the ice version-mismatch reload was added.)
3. Only when the symptom is genuinely orthogonal to existing stages, add a new
   stage gated by `STAGE_*=…` with a sensible default.
4. Add or update a row in the parent instruction's failure table.

Do **not** paste the workaround in chat — the next agent will rediscover it.

**Never edit the script (or anything else) to bypass a library bug.** If a real
MTL/DPDK/ice bug surfaces (segfault, deadlock, wrong output), diagnose it,
surface a clear repro to the human, and — if it's a known class — link the
failure-table row. Workaround/shim stages that mask library defects are not
acceptable: they hide regressions and pollute setup logs.

## Output (always end your turn with this)

```text
## Setup summary
- Stages: <name>=<ok|skip|fail|would-install> [<seconds>s] (one line per stage from script summary)
- Detected: PF=<vendor:device @ BDF>, hugepages=<X MiB free>, SSH-to-root=ok
- ice driver: <version> @ <module path>  (must be out-of-tree Kahawai_<ICE_VER>)
- NFS: NFS_SOURCE=<value-asked-from-user>, mounted at /mnt/media (<N entries>),
       canonical media file present=<yes|no>
- Configs: tests/validation/configs/{topology,test}_config.yaml
- Run log: /tmp/setup_validation-<UTC>.log
- Collect-only: <N items>, no errors

## Recommended pytest invocation (parent agent)
cd tests/validation && sudo -E ./venv/bin/python3 -m pytest \
  --topology_config=configs/topology_config.yaml \
  --test_config=configs/test_config.yaml \
  <selector> --tb=short -v

## Open items / asks
- <if any>
```

Always include `NFS_SOURCE` verbatim in the summary so the next agent can
inherit it without re-prompting the user.
