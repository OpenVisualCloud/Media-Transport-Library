# Move MTL to DPDK 26.07

## Goal

1. Carry as few DPDK/ICE patches as possible.
2. Prove the move with real testing, not the appearance of it.

Full work list and evidence: [`tasks.md`](tasks.md). Source record: [`upstreaming.md`](upstreaming.md).

## Critical path (in order)

- [ ] **T-03** Bump `versions.env` to DPDK 26.07. Irreversible for the gtest tier.
- [ ] **T-05** Capture the 26.03 hardware baseline. *(in progress)*
- [ ] **T-04** Add `rl_burst_size` to `struct mtl_port_init_params`. *(blocked)*
- [ ] **T-35** Let a shipped binary set `rl_burst_size` so T-06 can exercise it.
- [ ] **T-06** Verify the bump on real hardware (`--pacing_way rl`).
- [ ] **T-07** Run the acceptance A/B: old tree vs new tree, one host, one variable.

## Fewer patches (goal 1)

- [ ] **T-11** Move Rx to `RTE_ETH_RX_OFFLOAD_TIMESTAMP`, delete the PTP patch.
- [ ] **T-12** Move header split to `RTE_PKTMBUF_POOL_F_PINNED_EXT_BUF`. *(do not start)*
- [ ] **T-37** Hold one canonical ICE patch set, not 11 copies.

## Test holes to close (goal 2)

- [ ] **T-06** No gtest sets `--pacing_way rl`; the PF rate-limit path is uncovered.
- [ ] **T-36** ~~Rust example does not compile~~ **done**.
- [ ] **T-19** ~~Unit suite aborts after 46 of 513~~ **done**.

## Rules that void a careless run

- Never run the old and new acceptance suites at the same time on one host — one loader
  cache, one `mtl_local.conf`, last-writer-wins.
- Prove the DPDK version in-run: `--log_level notice`, grep `dpdk version:`.
- Compare the pass/fail **sets**, not the counts.
