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

### Command-Line

```bash
./build/tests/KahawaiTest \
    --auto_start_stop \
    --port_list=0000:xx:xx.x,0000:xx:xx.x,0000:xx:xx.x,0000:xx:xx.x \
    --gtest_filter=noctx.TestName \
    --no_ctx
```

Replace `TestName` with any test from [`noctx.env`](noctx.env) and update the port addresses to match your system.

### Environment configuration

You can store the PCI addresses once in `tests/integration_tests/noctx/noctx.env`; `run.sh` sources this file automatically before executing tests.

```bash
TEST_PORT_1=0000:xx:xx.x
TEST_PORT_2=0000:yy:yy.y
TEST_PORT_3=0000:zz:zz.z
TEST_PORT_4=0000:aa:aa.a
```

Exporting the same variables in your shell will continue to work, but keeping them in `noctx.env` avoids repeating the configuration.


### Running all tests

As the tests need to be run one after another to avoid the dpdk initialization
issues you can use a handy script
```bash
run.sh
```

to run all nocx tests

