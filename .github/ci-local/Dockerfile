# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Simulated GitHub Actions runner for the MTL workflows.
#
# The real `build` job runs on a self-hosted `dpdk` runner: Ubuntu 22.04, a
# non-root user with passwordless sudo, and every apt dependency already
# present from earlier runs. A bare `ubuntu:22.04` differs from that in three
# ways that each produce a failure which has nothing to do with the change
# under test:
#
#   * apt resolves the mirrors over IPv6, which many hosts cannot route
#   * `sudo` resets the environment, dropping the proxy variables and
#     DEBIAN_FRONTEND, so tzdata stops on an interactive prompt
#   * every run re-downloads ~400 packages
#
# This image removes all three, so a failure inside it is a real failure.

FROM ubuntu:22.04

# Matches the timezone setup_environment.sh applies under CICD_BUILD=1.
ARG TZ=Europe/Warsaw
# The container runs as the invoking host user so that files it creates in the
# bind-mounted workspace stay owned by that user.
ARG RUNNER_UID=1000
ARG RUNNER_GID=1000
ARG RUNNER_USER=runner
ARG http_proxy=""
ARG https_proxy=""
ARG no_proxy=""

ENV DEBIAN_FRONTEND=noninteractive \
	TZ=${TZ} \
	HOME=/home/runner \
	PIP_BREAK_SYSTEM_PACKAGES=1

# apt: no IPv6, honour the build-time proxy, no interactive tzdata.
RUN set -eux; \
	echo 'Acquire::ForceIPv4 "true";' >/etc/apt/apt.conf.d/99force-ipv4; \
	if [ -n "${http_proxy}" ]; then \
	printf 'Acquire::http::Proxy "%s";\nAcquire::https::Proxy "%s";\n' \
	"${http_proxy}" "${https_proxy:-${http_proxy}}" >/etc/apt/apt.conf.d/99proxy; \
	fi; \
	ln -fs "/usr/share/zoneinfo/${TZ}" /etc/localtime; \
	echo "${TZ}" >/etc/timezone

# Pre-warm the dependency set that setup_environment.sh installs. The script
# still runs apt at job time; anything already present is a no-op, so this list
# drifting behind the script costs a download, never correctness.
#
# It deliberately goes further than the script does, because the self-hosted
# runner does too: packages an earlier run installed under a flag that is off
# today are still on the machine. libelf-dev is the clearest case -- libdpdk.pc
# requires it, yet nothing installs it unless SETUP_BUILD_AND_INSTALL_EBPF_XDP
# is set, so a DPDK cache hit on a fresh machine fails where the runner passes.
RUN set -eux; \
	apt-get update; \
	apt-get install -y --no-install-recommends \
	build-essential \
	ca-certificates \
	clang \
	cmake \
	curl \
	doxygen \
	ethtool \
	gcc \
	gcc-multilib \
	git \
	gstreamer1.0-libav \
	gstreamer1.0-plugins-base \
	gstreamer1.0-plugins-good \
	gstreamer1.0-tools \
	libcap-ng-dev \
	libcap2-bin \
	libelf-dev \
	libgstreamer-plugins-base1.0-dev \
	libgstreamer1.0-dev \
	libgtest-dev \
	libjson-c-dev \
	libnuma-dev \
	libpcap-dev \
	libsdl2-dev \
	libsdl2-ttf-dev \
	libssl-dev \
	libunwind-dev \
	linuxptp \
	llvm \
	locales \
	m4 \
	make \
	meson \
	nasm \
	netsniff-ng \
	ninja-build \
	patch \
	pkg-config \
	python3 \
	python3-dev \
	python3-pip \
	python3-venv \
	sudo \
	systemtap-sdt-dev \
	tzdata \
	unzip \
	wget \
	yasm \
	zlib1g-dev; \
	dpkg-reconfigure -f noninteractive tzdata; \
	python3 -m pip install --no-cache-dir --upgrade pip; \
	python3 -m pip install --no-cache-dir pyelftools ninja; \
	rm -rf /var/lib/apt/lists/*

# A non-root user with passwordless sudo, like the GitHub runner. sudo's
# env_reset would otherwise strip the proxy and DEBIAN_FRONTEND that every apt
# call in setup_environment.sh depends on.
RUN set -eux; \
	groupadd --gid "${RUNNER_GID}" --non-unique "${RUNNER_USER}"; \
	useradd --uid "${RUNNER_UID}" --gid "${RUNNER_GID}" --non-unique \
	--home-dir "${HOME}" --create-home --shell /bin/bash "${RUNNER_USER}"; \
	echo "${RUNNER_USER} ALL=(ALL) NOPASSWD:ALL" >/etc/sudoers.d/10-runner; \
	echo 'Defaults env_keep += "DEBIAN_FRONTEND http_proxy https_proxy no_proxy HTTP_PROXY HTTPS_PROXY NO_PROXY PIP_BREAK_SYSTEM_PACKAGES"' \
	>/etc/sudoers.d/20-env; \
	chmod 0440 /etc/sudoers.d/10-runner /etc/sudoers.d/20-env

USER ${RUNNER_UID}:${RUNNER_GID}
WORKDIR /github/workspace
