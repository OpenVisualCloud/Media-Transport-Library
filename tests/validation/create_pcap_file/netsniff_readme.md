# Automated packet capture using Netsniff-ng

## Downloading and installing the tool

> **Note:** At the moment, there is no automation present for netsniff-ng tool installation on the capturing machine. It must be installed on the system manually before running the test. If the capture fails, the test continues without the packet capture.

There is an official instruction on how to download the tool present on the [tool's site](http://netsniff-ng.org/). It can also be useful for finding mirrors of the repositories. Below, only the GitHub repository is considered as a source.

```shell
NETSNIFF_REPO="https://github.com/netsniff-ng/netsniff-ng"
NETSNIFF_VERSION="v0.6.9"
```

### Download the repository

1. Clone the repository
2. Change directory to the cloned repository
3. Checkout on the proper release branch

```shell
git clone "${NETSNIFF_REPO}" netsniff-ng # 1.
cd netsniff-ng # 2.
git checkout "${VERSION}" # 3.
```

Alternatively:

1. Download the release package
2. Unpack the package

```shell
wget "${NETSNIFF_REPO}/archive/refs/tags/${NETSNIFF_VERSION}.tar.gz" # 1.
tar xvz "${NETSNIFF_VERSION}" # 2.
```

### Install the repository

> **Note:** The [document](https://github.com/netsniff-ng/netsniff-ng/blob/main/INSTALL) is also available online (main branch)

1. Use the INSTALL document from repository root to install the tool


## Usage

After the tool is installed on the capturing machine, it can be used within the test code.

In order to create and receive a `NetsniffRecorder` class instance, that can be used to start and stop the packet capture,
use the `prepare_netsniff()` function. It takes the capture config (`capture_cfg`), host (`host`) and optional filters for source
and destination IP addresses (`src_ip` and `dst_ip` respectively). The filtering IPs should be used to avoid capturing unrelated traffic during the test.

To start the capture, call `start()` function within the `NetsniffRecorder` class object, with optional `startup_wait` parameter specifying the number of seconds that should be waited before the capture starts (initialization delay).

To capture for predetermined number of seconds, call the `capture()` function with `capture_time` parameter. It starts the capture in a blocking manner. This function can optionally pass the `startup_wait` parameter to the `start()` function.

Analogically, to stop the capture, call `stop()` function. This function stops the capture immediately.
