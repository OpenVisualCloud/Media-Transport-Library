# No Context tests

Integration tests need to be run one after another due to the fact that dpdk
does not support reinitialization in the same PID, this is achived by the
Kahawai tests in tests/integration_tests

General API tests are performed there.

For bugs and other purposes where there is a need for "isolated" evironment
another app was created called noctx, no contex tests take the arguemnt
logic from general tests but run it in an more isolated environment.

Useful if you wan't to control MTL intializaiton.

Those tests mostly will be used to "check" scenarios that proved to be buggy
and need special configuration.

## Running no context tests

Most tests only need a TX/RX port pair, but `run.sh` (the full-suite runner)
always requires all 4 ports, since a handful of redundant-session tests need
them — running with fewer ports would otherwise let those tests fail
ambiguously. Ports can be PFs or VFs bound to vfio-pci.

TSN / launch-time-pacing tests specifically require a PF, since that offload
is not advertised by VF drivers. Such tests are named with a `_pf_` infix
(e.g. `st20p_tx_epoch_onward_recovers_after_ptp_step_pf_tsn_pacing`) so they
can be selected or excluded with a plain `--gtest_filter` wildcard. `run.sh`
excludes them (they can never pass against its VF-capable ports); use
`run_pf.sh` to run just them against PF ports instead.

TSN launch-time-pacing has only been validated on Intel **E830** (PCI device
ID `0x12d2`); other NICs (e.g. E810, `0x1592`) may not advertise it or may
behave differently. `_pf_` tests are not gated on device ID in the test code
itself — run them on an E830 PF, and expect unrelated failures/hangs on other
hardware.

### Command-Line

```bash
./build/tests/KahawaiTest \
    --auto_start_stop \
    --port_list=0000:xx:xx.x,0000:yy:yy.y \
    --gtest_filter=noctx.TestName \
    --no_ctx
```

Replace `TestName` with any test from [`noctx.env`](noctx.env) and update the port addresses to match your system. Add two more comma-separated BDFs to `--port_list` for tests that need redundant sessions.

### Environment configuration

You can store the PCI addresses once in `tests/integration_tests/noctx/noctx.env`; `run.sh` and `run_pf.sh` source this file automatically before executing tests.

```bash
TEST_PORT_1=0000:xx:xx.x
TEST_PORT_2=0000:yy:yy.y
TEST_PORT_3=0000:zz:zz.z
TEST_PORT_4=0000:aa:aa.a
# Only needed by run_pf.sh, must be bound to vfio-pci as PFs (not VFs)
TEST_PF_PORT_1=0000:bb:bb.b
TEST_PF_PORT_2=0000:cc:cc.c
```

Exporting the same variables in your shell will continue to work, but keeping them in `noctx.env` avoids repeating the configuration.


### Running all tests

As the tests need to be run one after another to avoid the dpdk initialization
issues you can use a handy script
```bash
run.sh
```

to run all tests that don't need a PF (all `TEST_PORT_1..4` must be set).

### Running PF-only tests

Tests named with a `_pf_` infix require PF ports and are skipped by `run.sh`.
Run them separately with:
```bash
run_pf.sh
```

which requires `TEST_PF_PORT_1`/`TEST_PF_PORT_2` (2 PF ports covers all
current PF-only tests).

