# Automated packet capture using Netsniff-ng

## Downloading and installing the tool

> **Note:** At the moment, there is no automation present for netsniff-ng tool installation on the capturing machine. It must be installed on the system manually before running the test. If the capture fails, the test continues without the packet capture.

There is an official instruction on how to download the tool present on the [tool's website](http://netsniff-ng.org/). It can also be useful for finding mirrors of the repositories. Below, only the Github repository is considered as a source.

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
