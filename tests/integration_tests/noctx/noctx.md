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
    --gtest_filter=noctx.TestName
```

Replace `TestName` with any test from [`noctx.env`](noctx.env) and update the port addresses to match your system.


### Running all tests

As the tests need to be run one after another to avoid the dpdk initialization
issues you can use a handy script
```bash
run.sh
```

to run all nocx tests

