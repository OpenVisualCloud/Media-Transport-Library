# MTL Validation Tests

This folder contains validation tests for [Media Transport Library](https://github.com/OpenVisualCloud/Media-Transport-Library).

Full documentation in folder [`doc`](doc/README.md)

## Legacy tests

### API tests

Folder `api`.
Old engine created to execute KahawaiTest. This is a suite of unit tests written in GTest.

### Functional tests

Folder `functional`.
Old engine created to execute RxTxApp. This is a sample app to test API functions.

### Invalid tests

Folder `invalid`.
Old engine created to execute RxTxApp. This is a sample app to test API functions.

## Pytest tests

Folder `tests`. Logs from executions are available in `logs` folder. Latest results are in `logs/latest`.

Test engine is based on pytest.

Installation:

```bash
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Content of tests repository:

- invalid - negative tests to check library response to wrong values
- single - functional tests of different features. These tests are using single host and can be run on PF or VF.
  - dpdk - tests using DPDK driver
  - xdp - tests using XDP driver mode
  - kernel - tests using kernel sockets
- dual - functional, load and stress tests. These tests require dual host setup with switch.
