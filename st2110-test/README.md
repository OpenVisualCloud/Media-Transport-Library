# st2110-test — quick ST2110 test drivers

Helper scripts for the three test tiers that need a NIC. See `ANALYSIS.md` for the
`tasks.md` summary and how these map to the tiers. All three need two DPDK-bound VFs,
hugepages, and (for RxTxApp/acceptance) `MtlManager` running.

## 0. Host prep (once)

```bash
sudo ./script/nicctl.sh create_vf 0000:af:00.0   # create + bind VFs to the PMD
sudo sysctl -w vm.nr_hugepages=2048
sudo MtlManager &                                 # lcore/queue daemon
./script/nicctl.sh list                           # find your VF BDFs
```

## 1. Functionality tests — KahawaiTest (integration gtest)

```bash
./build.sh                                        # build lib + tests first
sudo ./st2110-test/run-kahawai.sh 0000:af:01.0 0000:af:01.1
# software-pacing fallback / rate-limit path:
sudo PACING=tsc ./st2110-test/run-kahawai.sh 0000:af:01.0 0000:af:01.1
sudo PACING=rl  FILTER='St20p*' ./st2110-test/run-kahawai.sh   # the T-04/T-06 path
```

## 2. RxTxApp loopback (TX VF0 -> RX VF1, one process)

```bash
sudo ./st2110-test/run-rxtxapp-loopback.sh st20p 0000:af:01.0 0000:af:01.1
sudo ./st2110-test/run-rxtxapp-loopback.sh st30p 0000:af:01.0 0000:af:01.1   # audio
sudo TEST_TIME=30 MEDIA=/mnt/media/some.yuv ./st2110-test/run-rxtxapp-loopback.sh st20p
```

Synthesizes zero-filled media of the correct frame size when `MEDIA` is unset, so no
NFS mount is needed for a data-plane smoke.

## 3. Acceptance tests (pytest E2E) — set up the framework, then smoke

```bash
sudo ./st2110-test/setup-acceptance.sh --status                 # read-only host report
sudo -E ./st2110-test/setup-acceptance.sh 0000:c9:00.0 10.0.0.5:/mnt/NFS/mtl_assets/media
sudo -E ./st2110-test/setup-acceptance.sh --run-only 0000:c9:00.0   # rerun smoke only
```

Setup builds the separate `.local_install` tree (DPDK+MTL), mounts NFS `/mnt/media`,
sets up passwordless SSH to `root@127.0.0.1`, creates the venv, and generates the two
config YAMLs — then runs `pytest -m smoke`. It **mutates the host**.

## Rules baked in (from `tasks.md`)

- Every run prints the loaded DPDK version (`--log_level notice`, grep `dpdk version:`) —
  a run that cannot name what it loaded proves nothing.
- Never run two acceptance install trees at once: one loader cache, last-writer-wins.
