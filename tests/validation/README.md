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

Installation can be done in 2, similar in outcome, ways:

Approach using pure venv:

```bash
# Install mandatory system packages
sudo apt update
sudo apt install -y python3-dev python3-virtualenv python3-venv python3-pip
# Create virtual environment (venv) for python3
python3 -m venv .venv
# Activate venv for python3
source .venv/bin/activate
# Install required python3 modules
python3 -m pip install -r requirements.txt
# User now can run command like pytest using:
sudo --preserve-env python3 -m pytest
```

Approach using pipenv:

```bash
# Install mandatory system packages
sudo apt update
sudo apt install -y python3-dev python3-virtualenv python3-venv python3-pip
# Install pipenv for python3
python3 -m pip install pipenv
# Install venv using pipenv for python3
python3 -m pipenv install -r requirements.txt
# User now can run command like pytest using:
sudo --preserve-env python3 -m pipenv run pytest
# User can also activate pipenv shell to have it persistent:
sudo --preserve-env python3 -m pipenv shell
```

Testcases should be run using `sudo` or root account. The basic and minimal approach command takes `--nic="${TEST_PORT_P},${TEST_PORT_R}"` as input.
Self-hosted runners will have an media directory that is available under `/mnt/media` path (this is the default value for media parameter: `--media=/mnt/media`).
Simple example when being run from tests/validation subdirectory will look like for pipenv:

```bash
sudo \
  --preserve-env python3 \
  -m pipenv run pytest \
  --nic="${TEST_PORT_P},${TEST_PORT_R}" \
  --media=/mnt/media \
  --build="../.."
```

Content of tests repository:

- invalid - negative tests to check library response to wrong values
- single - functional tests of different features. These tests are using single host and can be run on PF or VF.
  - xdp
  - kernel
  - ancillary
  - audio
  - dma
  - ffmpeg
  - gstreamer
  - kernel_socket - tests using kernel sockets
  - performance
  - ptp
  - rss_mode
  - rx_timing
  - st20p
  - st22p
  - st30p
  - st41
  - udp
  - video
  - virtio_user
  - xdp - tests using XDP driver mode
- dual - functional, load and stress tests. These tests require dual host setup with switch.
