# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

# NOTE: This Dockerfile is intended for development purposes only.
# It has been tested for functionality, but not for security.
# Please review and modify as necessary before using in a production environment.

# Ubuntu 22.04, build stage
FROM ubuntu@sha256:149d67e29f765f4db62aa52161009e99e389544e25a8f43c8c89d4a445a7ca37

LABEL maintainer="andrzej.wilczynski@intel.com,dawid.wesierski@intel.com,marek.kasiewicz@intel.com"

# Install build dependencies and debug tools
RUN apt-get update -y && \
    apt-get install -y --no-install-recommends clang-format-14 black isort && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

ENV MTL_REPO=Media-Transport-Library

LABEL maintainer="andrzej.wilczynski@intel.com,dawid.wesierski@intel.com,marek.kasiewicz@intel.com"

# Create the mtl user and switch to it
RUN useradd -ms /bin/bash mtl
USER mtl
WORKDIR $MTL_REPO
CMD ["/bin/bash"]

