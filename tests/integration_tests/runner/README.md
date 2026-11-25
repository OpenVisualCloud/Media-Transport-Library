# Integration Test Runner (`KahawaiTest`)

Replaces `.github/scripts/gtest.sh`, `noctx/run.sh`, and `noctx/run_pf.sh` with
one shell NIC helper and one Python runner.

- **`nic_setup.sh`**: Performs only `create-tvf <pf_bdf> [n]`, `bind-pvf
  <bdf...>`, and matching cleanup (`restore-pvf`, `disable-vf`, `vf-rss-lut`).
- **`gtest_runner.py`**: Pure Python stdlib runner. Discovers NICs and hugepages
  from sysfs, runs ordered PR and nightly profiles (including per-case `noctx`
  VF and `noctx_pf` PF suites), retries only failed cases individually
  (recording `flaky`), exits `3` on host faults, and writes `results.jsonl` and
  JUnit XML.

```bash
# TVF + PVF dry run
python3 tests/integration_tests/runner/gtest_runner.py \
  --tvf-pf 0000:4b:00.0 --bind-pvf 0000:31:00.0,0000:31:00.1 --dry-run

# Run PF-only tests directly on passed PCI device IDs
python3 tests/integration_tests/runner/gtest_runner.py \
  --profile '^noctx_pf$' --bind-pvf 0000:31:00.0,0000:31:00.1

# Offline self-test harness (runner + nightly workflow contracts)
python3 -m pytest -q -p no:cacheprovider tests/integration_tests/runner/
```
