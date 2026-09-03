# Clean-host quickstart

This is the shortest complete path from two clean Ubuntu hosts to a measured
FFmpeg-MXL run. Run every command from the repository root on the control-plane
host unless noted otherwise.

The installer changes both hosts: it sets their hostnames, disables swap,
installs and configures containerd and Kubernetes, and restarts the worker's
kubelet while applying CPU QoS. Use dedicated lab hosts.

> **Troubleshooting first:** If your network needs a proxy, set
> `LAB_HTTP_PROXY`/`LAB_HTTPS_PROXY` in `config/lab.env` before running this guide.
> Scripts build `NO_PROXY` for cluster addresses automatically, but `kubectl`
> commands you type manually may still need shell `no_proxy`/`NO_PROXY`. See
> [02-kubernetes-install-troubleshooting.md#proxy-and-no_proxy-failures](02-kubernetes-install-troubleshooting.md#proxy-and-no_proxy-failures).

## 1. Prepare access

Requirements:

* Ubuntu 22.04 or 24.04 on both hosts.
* One control-plane host and one two-socket Intel Xeon worker.
* The same SSH login on both hosts. Use `root`, or a user with `sudo` access.
* Passwordless SSH from the control-plane host to both hosts, including itself.
* A local 1080p60 MP4 input clip.

On the control-plane host:

```bash
sudo apt-get update
sudo apt-get install -y git curl python3 python3-venv make
curl -fsSL https://raw.githubusercontent.com/helm/helm/main/scripts/get-helm-3 | bash

ssh-keygen -t ed25519                    # skip when a key already exists
ssh-copy-id <login>@<control-plane-ip>
ssh-copy-id <login>@<worker-ip>
```

Both checks must print `ok` without asking for a password:

```bash
ssh -o BatchMode=yes <login>@<control-plane-ip> 'echo ok'
ssh -o BatchMode=yes <login>@<worker-ip> 'echo ok'
```

Passwords are never stored by this repository. Password prompts from
`ssh-copy-id` and `sudo` are handled directly by SSH and the remote host.

## 2. Configure three values

Edit `config/nodes.env`:

```bash
CONTROL_PLANE_HOST=<control-plane-ip>
WORKER_1_HOST=<worker-ip>
LAB_SSH_USER=<login>
```

Use real addresses reachable between the hosts, not `localhost` or SSH aliases.
The default Kubernetes node names are `control-plane` and `worker-1`.

Check the worker topology:

```bash
ssh <login>@<worker-ip> "lscpu | grep -E 'Socket|Core|Thread|NUMA node\('"
ssh <login>@<worker-ip> "lscpu -p=CPU,CORE,NODE | grep -v '^#' | head -4"
```

Update `LAB_SOCKET_COUNT`, `LAB_CORES_PER_SOCKET`, `LAB_THREADS_PER_CORE`,
`LAB_CPU_NUMBERING`, `LAB_SOCKET0_PARITY`, and `LAB_RESERVED_CPUS` in
`config/lab.env`.
Set `LAB_CPU_NUMBERING=contiguous` when each socket owns one consecutive CPU-ID
range. See [02-kubernetes-install.md](02-kubernetes-install.md) for both models.
Set `LAB_THREADS_PER_CORE` to what `lscpu` reports — `2` with Hyper-Threading on
is supported, and then `LAB_RESERVED_CPUS` has to cover both siblings of every
reserved core.

### Direct internet access

Leave `LAB_HTTP_PROXY` and `LAB_HTTPS_PROXY` commented out. Remove stale proxy
variables from the shell if this host no longer uses them:

```bash
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY no_proxy NO_PROXY
```

### Proxy access

Set the proxy once in `config/lab.env`:

```bash
LAB_HTTP_PROXY=http://proxy.example.com:port
LAB_HTTPS_PROXY=http://proxy.example.com:port
```

Also export it in the current shell so the initial `git`, `curl`, Helm, and pip
downloads work:

```bash
source config/lab.env
export http_proxy="$LAB_HTTP_PROXY" https_proxy="$LAB_HTTPS_PROXY"
```

The installer configures apt, containerd, kubeadm, and kubectl on both nodes. It
builds `NO_PROXY` from the node addresses and Kubernetes CIDRs. Do not add proxy
credentials to a commit.

## 3. Install the cluster and workload

Run these commands in order. Every installer is safe to rerun after correcting a
failure.

```bash
scripts/check-bios.sh
scripts/setup-controller-worker-ssh.sh
scripts/install-k8s-cluster.sh
scripts/configure-cpu-qos.sh
scripts/configure-power.sh
scripts/bootstrap-worker.sh
scripts/install-observability.sh
scripts/build-ffmpeg-mxl-image.sh
scripts/stage-media.sh /path/to/1080p60-input.mp4
scripts/setup.sh
scripts/preflight.sh
```

`scripts/configure-power.sh` sets the P-state driver, the scaling governor and
EPB/EPP/ELC on the worker from `config/lab.env`, and installs the unit that
re-applies them after a reboot. It asks for the worker's `sudo` password. Skipping
it does not stop the workload, but a `powersave` governor typically costs 1–2
streams and shows up nowhere except a lower score — see
[04-perfspect-baseline.md § Set the power profile](04-perfspect-baseline.md#set-the-power-profile).

Before staging media, set `LAB_INPUT_1080P` in `config/lab.env` to the input
file's basename. `preflight.sh` must finish with `Preflight passed.` and no
`MISS` lines. It confirms that prerequisites and the named file exist; the
one-stream smoke test below validates the clip and complete MXL data path.

`scripts/run-perfspect.sh` is recommended once per platform but is not required
to execute the workload. It records the hardware and firmware baseline under
`results/perfspect/`. Run it after preflight passes:

```bash
scripts/run-perfspect.sh
```

## 4. Smoke test, then measure

First prove that one complete decoder-to-encoder MXL flow works:

```bash
scripts/run.sh baseline --streams 1 --warmup 10s --measure 30s
```

Then run the three reference placements:

```bash
scripts/run-campaign.sh campaigns/density.env
```

The runner opens its own temporary Prometheus port-forward. Results are written
to `results/<run>/`, and the campaign builds:

* `results/summary.html`
* `results/summary.xlsx`
* `results/summary.csv`

The reference stream counts are starting points, not universal expectations.
Run `scripts/run-campaign.sh campaigns/density-sweep.env` to find the limit of a
different worker.

## Failure rule

Stop at the first failed step and fix the message immediately above it. Do not
continue to a measurement with a failed preflight. The installers are
idempotent, so rerun the same command; no reset is needed unless the Kubernetes
installer explicitly reports a half-finished `kubeadm init` and prints reset
instructions.

The numbered documents cover BIOS rationale, non-reference CPU topology,
observability, noisy neighbors, Intel RDT, and profiling after this basic path
works.