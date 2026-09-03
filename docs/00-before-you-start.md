# 0. Before you start

Complete this page before starting [01-bios-bkc.md](01-bios-bkc.md). It describes the
required setup for the rest of the guide and separates it from optional alternatives
and later requirements.


## Required before continuing

You need all of the following before starting Chapter 1:

- Two clean Ubuntu 22.04 or 24.04 hosts: one control-plane host and one Xeon worker.
- The same login name on both hosts.
- SSH key authentication from the control-plane host to itself and to every worker,
  with no password prompt.
- `sudo` access for that login, or `LAB_SSH_USER=root` for an unattended install.
- This repository checked out on the control-plane host.
- `config/nodes.env` filled in with the addresses and SSH login.

You do **not** need to install Kubernetes, `kubectl`, containerd, or Python manually
on the worker. Chapter 2 installs the node software over SSH. `kubectl` is also
installed by Chapter 2 on the control-plane host.

## Optional or needed later

These items are not required to complete the initial setup:

- `make` — optional convenience; every `make` target has an equivalent script.
- Helm — needed later in Chapter 6, not for the initial host setup.
- A proxy — only needed when the network requires one.
- Git — optional if you download the repository as a ZIP instead.
- A third machine for the repository — supported, but the repository must be on the
  control-plane host from Chapter 3 onward, where the kubeconfig and `results/` live.
- Running host-local checks directly on the worker — supported as described below.

If you skipped this page, the first script you run says so:

```
scripts/check-bios.sh: line 19: LAB_SSH_USER: set LAB_SSH_USER in config/nodes.env
```

That means `config/nodes.env` has not been completed yet.

## 1. The two hosts

| Role | What it is | Required capabilities |
|---|---|---|
| **Control-plane host** (`control-plane`) | The Kubernetes control-plane node and where you work. It never runs the workload, so a modest machine is fine. | Ubuntu 22.04 or 24.04, `sudo`, this repo |
| **Worker** (`worker-1`) | The Xeon machine under test. Its specification is what the results describe. | Ubuntu 22.04 or 24.04, `sudo`, Intel RDT |

More workers only means more entries in step 4. Both hosts can be freshly installed;
nothing else has to be prepared by hand.

You need **one login that exists on both hosts, with the same name**, able to become
root. Every script uses exactly that login and never stores a password.

Choose one access mode:

- **non-root user (Recommended):** use a normal login with `sudo`. SSH uses keys and must not prompt;
  `sudo` may prompt for the host's password during installation.
- **root user (Unattended option):** set `LAB_SSH_USER=root`. The scripts omit `sudo` and stop
  requesting a tty, so the installation does not prompt.

Passwordless SSH and passwordless `sudo` are different requirements:

| Access | Required? | Prompt allowed? |
|---|---:|---:|
| SSH key authentication | Yes | No |
| `sudo` access | Yes, unless using root | Yes for a normal login |
| Passwordless `sudo` | No | Only useful for unattended operation |

Steps 2, 3, and 4 happen in one place: **on the control-plane host, as that login.**

## 2. Required tools and repository location

Run the following on the control-plane host:

```bash
sudo apt-get update
sudo apt-get install -y git curl python3 python3-venv
```

The packages have these purposes:

- `git` and `curl` — obtain the repository and download installation resources.
- `python3-venv` — lets `scripts/setup.sh` build the runner's virtual environment.

`make` is optional. Install it only if you want to use the Make targets:

```bash
sudo apt-get install -y make
```

Helm is needed later by Chapter 6 for Prometheus and Grafana, so it can be installed
now or immediately before that chapter:

```bash
curl -fsSL https://raw.githubusercontent.com/helm/helm/main/scripts/get-helm-3 | bash
```

`sudo snap install helm --classic` works too.

Put the repository in your home directory and stay in it. Unless a later chapter
says otherwise, commands in this guide are run from the repository root:

```bash
git clone <this repository> ~/mxl-k8s-qos-lab
cd ~/mxl-k8s-qos-lab
```

The worker needs no packages installed by hand; the installers copy what they need
there and run it over SSH.

### Optional: use a downloaded ZIP

If Git is not available, a downloaded ZIP is equally fine:

```bash
sudo apt-get install -y unzip
unzip <branch>.zip && cd CRM-k8s-refrence-recipe-*
```

### Optional: keep the repository on a third machine

This works for Chapters 1 and 2 because both installers reach every node over SSH,
including the control-plane host. From Chapter 3 onward, the repository must be on
the control-plane host so the kubeconfig, Python runner, and `results/` are local.
You can copy it across with `scripts/sync-controller.sh`.

## 3. Required SSH access

Run these commands on the control-plane host as the same login configured in
`LAB_SSH_USER` — not as root, because the key must belong to the login used by the
scripts.

Almost every script reaches a host over SSH non-interactively (`BatchMode`), so SSH
password prompts must be gone **before** you start. Sudo passwords are different:
a normal login may be prompted by the host when installation reaches it, and the
password is never stored.

```bash
ssh-keygen -t ed25519 -C mxl-lab              # only if you have no key yet
ssh-copy-id <login>@<control-plane IP>        # yes, to this host itself
ssh-copy-id <login>@<worker IP>               # once per worker
```

**The first command is not a typo.** It copies your key into your own
`~/.ssh/authorized_keys`. The installer treats the control-plane host like any
other node and reaches it the same way, so without this step Chapter 2 fails on the
first host.

Verify the required SSH path now. Each command must print `ok`, with no prompt:

```bash
for host in <control-plane IP> <worker IP>; do
  ssh -o BatchMode=yes "<login>@$host" 'echo ok'
done
```

If either command asks for a password or reports `Permission denied (publickey)`,
stop and fix SSH before continuing. Every later script uses the same connection
method.

There is also a second, different hop: control-plane host → worker at run time.
The RDT helper and host noisy neighbor use it. Chapter 2 configures that hop with
`scripts/setup-controller-worker-ssh.sh`; you do not prepare it by hand.

## 4. Required fill in `config/nodes.env`

**The only file you must edit before Chapter 1 is `config/nodes.env`.**
`config/lab.env` ships with working defaults and does not need to be touched
unless you are changing node names, roles, or topology.

### Normal setup (default node names)

Open `config/nodes.env` in the repository on the control-plane host and fill in
these three values:

```bash
CONTROL_PLANE_HOST=<the control-plane IP or hostname>
WORKER_1_HOST=<the worker IP or hostname>
LAB_SSH_USER=<the login from step 1>
```

> **Address requirements**
>
> Use the address each host is reachable at **from the other host**. Give the
> control-plane host its real network address even though you are working on it.
> `localhost` and `127.0.0.1` are wrong: these values are written into every
> node's `/etc/hosts` and the control-plane address becomes the API server's
> advertise address. A DNS name that resolves the same on both hosts is fine; a
> `~/.ssh/config` alias is not — the installer detects and rejects it.

### How the variable names are derived

Each `_HOST` key is the Kubernetes node name, uppercased, with `-` replaced by
`_`, plus `_HOST`:

| Kubernetes node name | `config/nodes.env` key |
|---|---|
| `control-plane` | `CONTROL_PLANE_HOST` |
| `worker-1` | `WORKER_1_HOST` |
| `lab-w1` *(example)* | `LAB_W1_HOST` |

The shipped defaults in `config/lab.env` use `control-plane` and `worker-1`, so
the two keys above cover the default setup with no further changes.

### Optional: using your own node names

Edit `config/lab.env` **only** if you want different Kubernetes node names or
roles. Change `LAB_CONTROLLER`, `LAB_WORKERS`, and `LAB_DEFAULT_NODE` to your
chosen names, then add the corresponding `_HOST` keys to `config/nodes.env`
using the derivation rule above.

### Optional: keeping a host's existing hostname

Chapter 2 sets each host's hostname to its Kubernetes node name. If a host
already has a purpose and you need to keep its current hostname, set that
hostname as the node name in `config/lab.env` and use the matching `_HOST` key
in `config/nodes.env`.

## 5. Required readiness check

Do not start Chapter 1 until this check prints one `ok` line per host. Run it on
the control-plane host from the repository root:

```bash
source config/nodes.env
for var in $(grep -o '^[A-Z0-9_]*_HOST' config/nodes.env); do
  ssh -o BatchMode=yes "$LAB_SSH_USER@${!var}" "echo $var ok"
done
```

Expected output is similar to:

```text
CONTROL_PLANE_HOST ok
WORKER_1_HOST ok
```

Common failures:

- `LAB_SSH_USER` is unset — fill in `config/nodes.env` or source it again.
- `Permission denied (publickey)` — SSH key setup is incomplete.
- `Could not resolve hostname` — check the address in `config/nodes.env`.
- A sudo password prompt — expected for a normal sudo user; it is not an SSH
  failure.

You are ready to start [01-bios-bkc.md](01-bios-bkc.md) when every host responds
without an SSH password prompt.

## 6. Optional: Proxy configuration (only when needed)

Skip this section if the hosts have direct internet access. If your network uses a
proxy, set these two values in `config/lab.env`:

```bash
LAB_HTTP_PROXY=http://<proxy>:<port>
LAB_HTTPS_PROXY=http://<proxy>:<port>
```

The cluster installer configures `apt`, containerd, `kubeadm`, and `kubectl` from
these values on every node. Also set `http_proxy` and `https_proxy` in your shell
and for `apt` on the control-plane host, because Git, `curl`, and Helm need them.
You do not need to calculate `no_proxy` for the cluster: each script excludes the
cluster's own addresses for its own process. `scripts/preflight.sh` reports if your
shell needs the same exclusions for `kubectl` commands you type by hand.

For more detail or troubleshooting, see [02-kubernetes-install.md § Behind a
proxy](02-kubernetes-install.md#behind-a-proxy).

## 7. Network access and later requirements

The installers download from the internet. Before Chapter 2, both hosts need
access to the Ubuntu archives, `download.docker.com`, `pkgs.k8s.io`, and
`registry.k8s.io`. Later chapters also use the following:

- `raw.githubusercontent.com` — the Calico operator manifest.
- `quay.io` — the Tigera operator image.
- `docker.io` — Calico's own images. These are separate from `registry.k8s.io`;
  blocking either registry can leave nodes unable to become `Ready`.
- GitHub releases — nerdctl, Intel PerfSpect, and Intel PCM sources.
- `github.com/cbcrc` — FFmpeg-MXL build guidance for Chapter 5.
- `prometheus-community.github.io` and `ghcr.io` — Helm charts and the stress-ng
  image for Chapter 6.

A fully air-gapped install is not supported.

The hosts must also permit Calico's VXLAN data plane between every pair of
Kubernetes node addresses: **UDP port 4789 in both directions**. The installer uses
each Node's Kubernetes `InternalIP` for the tunnel endpoint, including hosts with
multiple network interfaces. A blocked VXLAN path can leave every Node and Calico
Pod reporting `Ready` while cross-node Pod traffic times out.

Every host must synchronize time over NTP, normally UDP port 123. The node installer
enables `chrony`, and preflight rejects more than five seconds of
control-plane-to-worker skew. Measurement windows and Prometheus samples cannot be
compared reliably across unsynchronized hosts.

## What is not in this repo

One 1080p60 source clip. Media is not shipped here; see
[05-ffmpeg-mxl-container.md](05-ffmpeg-mxl-container.md) for what to supply and
where it goes.

## Where things live

| Path | What it is |
|---|---|
| `config/nodes.env` | Addresses and the SSH login — the file you just filled in |
| `config/lab.env` | Topology and every default, commented |
| `scripts/` | One script per chapter of this guide, all idempotent |
| `docs/` | This guide, in order |
| `results/` | One directory per run, plus `summary.html` |

Next: [01-bios-bkc.md](01-bios-bkc.md).
