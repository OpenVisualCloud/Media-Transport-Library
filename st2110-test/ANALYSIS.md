# tasks.md — simple analysis

Snapshot of `tasks.md` (2026-08-26). 121 unique tasks (`T-NN`), tracking the **DPDK 26.07 move**.

## Status at a glance

| State | Count | Meaning |
|---|---:|---|
| Open | ~74 | Not started or in flight |
| Done | 29 | Closed (kept for the record, e.g. T-19, T-36, T-01/02/08/09/10/33) |
| Blocked | 5 | Waiting on a person or a precondition (T-04, T-39, T-82, T-85, T-118) |
| In review | 4 | At Gate 5/6 (T-73, T-123, T-127, T-114) |
| Cancelled | 6 | Dropped by decision D9/D10 |
| Needs user | 2 | Decision only (T-120, T-113) |

## The two goals

1. **Carry fewer patches** — only **T-11, T-12, T-37** can lower the count (11 DPDK + 5 ICE×11 dirs). Everything else holds it steady.
2. **Real testing, not the look of it** — every behaviour change gets a test at the cheapest tier; every recorded run must prove the DPDK version in-run (`--log_level notice`, grep `dpdk version:`).

## Critical path (the actual 26.07 move, in order)

| Task | State | What it does |
|---|---|---|
| **T-03** | open | Bump `versions.env` to DPDK 26.07 — irreversible for the gtest tier |
| **T-05** | in progress | Capture the 26.03 hardware baseline |
| **T-04** | blocked | Add `rl_burst_size` to `struct mtl_port_init_params` |
| **T-35** | open | Let a shipped binary set `rl_burst_size` so T-06 can exercise it |
| **T-06** | open | Verify the bump on real hardware (`--pacing_way rl`) |
| **T-07** | open | Acceptance A/B: old tree vs new tree, one host, one variable |

## Themes (where the open work sits)

- **DOING — the move**: T-03, T-04, T-05, T-06, T-07, T-35, T-108.
- **Fewer patches**: T-11, T-12 (*do not start*), T-37.
- **Blocked on a person**: T-20, T-27, T-28, T-31 (patch authorship/metadata).
- **Test gaps (Goal 2)**: no `--pacing_way rl` gtest coverage (T-06), fuzz tier does not build (T-115), MCP unit suite unrun (T-64).
- **Patch-set hygiene**: T-21, T-32, T-41, T-42, T-46, T-62, T-107 — metadata and apply-cleanliness; none change the patch count.
- **Docs / STE / acceptance instructions**: the large T-8x–T-12x block — link resolution, marker naming, quickstart fixes. Low risk, high volume.

## Testing rules that void a careless run (from `tasks.md` RULES)

1. Never run the old and new acceptance suites at the same time on one host — one loader cache, one `mtl_local.conf`, last-writer-wins.
2. Prove the DPDK version **in-run**, never from the tree or config.
3. Compare the pass/fail **sets**, not the counts.
4. Two install trees switched by a symlink; never edit the suite (`const.py` hardcodes `PREFIX=.local_install`).

---

## The three testing tiers, and which script covers each

| Tier | Cost | NIC / root | Script here |
|---|---|---|---|
| Unit | free | none | `./build.sh unit` (upstream) — not scripted here |
| **Integration (KahawaiTest)** | VFs + root | yes | `run-kahawai.sh` |
| **RxTxApp loopback** | VFs + root | yes | `run-rxtxapp-loopback.sh` |
| **Acceptance (pytest E2E)** | VFs + root + NFS media | yes | `setup-acceptance.sh` |
