---
description: "System setup, host configuration, and test execution for MTL. Covers DPDK, ICE driver, hugepages, VFs, MtlManager, and integration tests. Loaded when working on library code, apps, integration tests, or setup scripts."
applyTo: "lib/**,app/**,tests/integration_tests/**,.github/scripts/**,.github/mcp/**"
---

# MTL System Setup — Agent Instructions

MCP tools are provided by the `mtl-system-setup` server (prefix `mcp_mtl-system-se_`).
Tool descriptions are injected into context automatically — this file covers
decision logic and domain knowledge that tool descriptions alone don't convey.

---

## Decision Trees

### "User rebooted" / "Setup system"
1. `system_status` → read the output for what's missing
2. `setup_after_reboot_auto` → hugepages + CPU governor + VFs on all PFs + MtlManager
3. If output warns about ICE driver → `ice_driver_rebuild` → `setup_after_reboot_auto` again (VFs destroyed)
4. If MTL not built or rebuild needed → `build_mtl` or `mtl_clean_rebuild`

### "Run integration tests"
1. Verify prerequisites **before** the first `run_gtest` call: `dpdk_devbind_status` (ports bound as VFs, not bare PFs — see below), MtlManager running (`manager_start` if not).
2. `run_gtest` with appropriate `gtest_filter` (e.g. `St20p*`, `St30*`)
3. If tests segfault → see "Test crashed" below
4. If RX session creation fails with multicast-join/IGMP errors → see "Multicast join / IGMP failure" below
5. Report pass/fail counts

### "Test crashed / segfault"
1. **SEGFAULT in `iavf_tm_node_add`** → `ice_driver_status` — almost always stock ICE
2. **SEGFAULT elsewhere** → `dmesg_tail` for kernel errors
3. `dpdk_status` → version mismatch?
4. `mtl_clean_rebuild` → stale build?

### "Multicast join / IGMP failure on RX session creation"
1. `dpdk_devbind_status` first — check if the port is a bare PF bound to `vfio-pci` with 0 VFs. E810/E830 must always run through VFs (see `nic_bind_pmd`'s own tool description: prefer `nic_create_vf` for E800 series); a bare PF-to-vfio-pci binding causes exactly this symptom.
2. Fix: `nic_bind_kernel` on the PF, then `nic_create_vf` (use `trusted=true` if the test needs multicast/promiscuous features).
3. Re-run `run_gtest` with the newly created VF BDFs as `p_port`/`r_port`.
4. This is an environment issue, not a code regression — confirm by reproducing on one unrelated, already-passing suite (e.g. `St30_rx.create_free_single`) only if you need extra certainty; don't use it as the primary diagnostic path, step 1 above is sufficient and faster.

### "Build failed"
1. **Permission errors / CMakeCache.txt** → `mtl_clean_rebuild`
2. **librte_*.so not found** → `sudo ldconfig` (or `dpdk_build` if DPDK not installed)
3. **Missing headers** → `install_dependencies`

---

## Key Facts

- **ICE driver**: Patched out-of-tree ICE (Kahawai_X.Y.Z) is REQUIRED for RL/TM pacing on VFs. Stock kernel ice does NOT advertise `VIRTCHNL_VF_OFFLOAD_QOS`.
- **DPDK version**: Must match `versions.env` `DPDK_VER`. Patches in `patches/dpdk/<ver>/` are applied during `dpdk_build`.
- **VFs destroyed on ICE reload**: Always re-create VFs after `ice_driver_rebuild`.
- **ldconfig**: Always run after installing DPDK or MTL — libraries won't be found otherwise.
- **MtlManager**: Must be running for lcore allocation. Start with `manager_start`.
- **E810 vs E830**: Both supported, same VF workflow. E830 device ID `12d2`, E810 device ID `1592`.
- **NoCtx `_pf_` tests (TSN/launch-time-pacing)**: only validated on E830 (`12d2`). Not gated by device ID in the test code — check the port's device ID yourself (e.g. `cat /sys/bus/pci/devices/<bdf>/device`) before running `run_pf.sh` / `run_noctx_pf_tests`; expect unrelated failures on E810 or other NICs.
- **NUMA**: Allocate from the same NUMA node as the NIC for best performance. VFs inherit PF's NUMA node.
- **VF BDF patterns**: PF `.0` creates VFs at `:01.0-5`, PF `.1` at `:11.0-5`.
- **versions.env**: Defines `DPDK_VER`, `ICE_VER`, `ICE_DMID`. Patches live in `patches/dpdk/<ver>/`.
