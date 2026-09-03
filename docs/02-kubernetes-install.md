# 2. Automated Kubernetes installation

One command turns bare Ubuntu hosts into the cluster this lab runs on. It is
idempotent: re-running skips whatever is already in place.

> **Troubleshooting first:** See
> [02-kubernetes-install-troubleshooting.md](02-kubernetes-install-troubleshooting.md)
> for common failures during this chapter, including proxy and image-pull issues.

## What this chapter does

Installs a two-node Kubernetes cluster (one control-plane, one worker) using
`kubeadm`, containerd, and the Calico CNI. By the end every node is `Ready` and the
cluster is ready for the CPU QoS configuration in the next chapter.

Kubernetes discovers CPU and NUMA topology from the Linux kernel automatically.
Kubelet, CPU Manager, and Topology Manager all obtain that information without any
manual input. Worker CPU pool configuration for the recipe's workload scheduling is a
separate concern; it is covered in [03-cpu-qos.md](03-cpu-qos.md).

## Prerequisites

* **Chapter 00-before-you-start complete** — tools installed, passwordless SSH from your machine to
  every host, and `config/nodes.env` filled in
  ([00-before-you-start.md](00-before-you-start.md)).
* **Chapter 01-bios-bkc complete** — BIOS settings applied; `Thread(s) per core: 1` (or `2`
  if you deliberately left SMT on — see step 1) and `NUMA node(s)` equal to
  `Socket(s)` when you run `lscpu` on the worker ([01-bios-bkc.md](01-bios-bkc.md)).

The minimum topology is two hosts: a control-plane node that never runs the
workload, and one worker that is the machine under test. The control plane can be a
modest machine; only the worker's specification matters for the results. Adding more
workers means adding more entries in `config/nodes.env`.

Node names are yours to choose. This guide uses `control-plane` and `worker-1`,
which are also the defaults shipped in `config/lab.env`. **The installer sets each
host's hostname to its node name**, so if you do not want your machines renamed put
their existing hostnames in `config/lab.env` first.

## Step 1 — confirm the cluster inventory

`config/nodes.env` was filled in as part of
[00-before-you-start.md](00-before-you-start.md). What is left is
`config/lab.env`: which hosts are the controller and the workers
```bash
LAB_CONTROLLER=control-plane
LAB_WORKERS=worker-1            # comma-separated if you have several
LAB_DEFAULT_NODE=worker-1       # the worker measurements are run on
```

## Step 2 — SSH keys, the second hop

You already have SSH from *your* machine to every host. The installer and later
scripts also need passwordless SSH **from the control-plane host** to each worker,
because the RDT helper and the host noisy-neighbor scripts run over that path. This
command generates that key and installs it:

```bash
scripts/setup-controller-worker-ssh.sh
```

It also gives the control-plane host a key to itself, which the installer needs when
you run it on that host.

## Step 3 — run the installer

```bash
scripts/install-k8s-cluster.sh
```

Run it from the repo root on the control-plane host, or on any machine with SSH key
access to both nodes. The installer prints a plan before doing anything — check that
the login and proxy lines say what you expect:

```
== plan ==
  controller: control-plane (<control-plane IP>)
  worker:     worker-1 (<worker IP>)
  kubernetes: 1.35.1-1.1    pod CIDR: 10.244.0.0/16    CNI: Calico (VXLAN)
  login:      root
  proxy:      none (set LAB_HTTP_PROXY in config/lab.env if this network needs one)
```

With `LAB_SSH_USER=root` the whole install is unattended. With a normal login every
stage that needs root uses `sudo`, and you are prompted for each node's password on
that node's own prompt. Five stages:

| Stage | What happens |
|---|---|
| 1/5 | Copies `scripts/install-k8s-node.sh` to every node and runs it there. Per node: proxy for apt / `/etc/environment` / containerd, hostname and `/etc/hosts`, swap off, `overlay`/`br_netfilter` + sysctls, containerd with `SystemdCgroup = true`, pinned `kubelet/kubeadm/kubectl` + `apt-mark hold`, registry reachability check, lab prerequisites (`numactl`, `stress-ng`, `dmidecode`, `/opt/mxl-media`, `/dev/shm/mxl`). |
| 2/5 | Pulls control-plane images, then `kubeadm init` on the controller with `--pod-network-cidr=10.244.0.0/16` and `--node-name`, then writes `~/.kube/config`. |
| 3/5 | Calico via the tigera-operator, then [cluster/calico-installation.yaml](../cluster/calico-installation.yaml) (VXLAN, BGP disabled). Applied server-side, so a re-run converges. |
| 4/5 | `kubeadm token create --print-join-command --kubeconfig=/etc/kubernetes/admin.conf`, then joins every worker with `--node-name`. |
| 5/5 | Waits for all nodes `Ready` and prints `kubectl get nodes -o wide`. |

Every stage is idempotent. If one fails the installer says so and confirms that
re-running is safe. Stage 4/5 asks the controller for the token with passwordless
`sudo -n` first and falls back to an interactive connection, so a normal login's
`sudo` prompt stays on your screen instead of the stage stopping on a blank line.

Defaults (overridable via environment variable or `config/lab.env`):

```bash
K8S_VERSION=1.35.1-1.1        # apt package version; the cluster becomes v1.35.1
POD_CIDR=10.244.0.0/16
SERVICE_CIDR=10.96.0.0/12
CALICO_OPERATOR_URL=.../calico/v3.32.0/manifests/tigera-operator.yaml
LAB_HTTP_PROXY=               # empty: direct. See "Behind a proxy" below
LAB_HTTPS_PROXY=              # defaults to LAB_HTTP_PROXY
SKIP_REGISTRY_CHECK=0         # 1 skips the test pull (mirror or pre-loaded images)
```

`K8S_VERSION` selects both the apt repository and the package version, so a value
from a different minor release works too:

```bash
K8S_VERSION=1.34.5-1.1 scripts/install-k8s-cluster.sh
```

### Behind a proxy

If your network requires an HTTP proxy, set it once in `config/lab.env`:

```bash
LAB_HTTP_PROXY=http://<proxy>:<port>
LAB_HTTPS_PROXY=http://<proxy>:<port>   # optional; defaults to LAB_HTTP_PROXY
```

The installer configures apt, containerd, `/etc/environment`, and kubeadm on every
node from those two values. You never need to compose a `NO_PROXY` yourself: the
installer builds one from `config/nodes.env` and writes it everywhere. If proxy
issues occur during installation, see
[02-kubernetes-install-troubleshooting.md](02-kubernetes-install-troubleshooting.md).

### Watching progress

Roughly 10–20 minutes on two hosts with a good link. Every stage header is stamped
with elapsed time, and silent steps tick every 15 seconds:

| Stage | Typical | Suspicious after |
|---|---|---|
| 1/5 per node | 2–6 min | 10 min |
| 2/5 images + `kubeadm init` | 2–8 min | 5 min with no new line |
| 3/5 Calico | 1–4 min | the 5 min / 10 min timeouts it carries |
| 4/5 join per worker | 1–2 min | 5 min |
| 5/5 status | seconds | — |

**`NotReady` right after stage 2/5 is expected.** The node goes `Ready` during
stage 3/5 when Calico is installed.

## Step 4 — verify the cluster

```bash
kubectl get nodes -o wide          # every node Ready, correct versions
kubectl get pods -A                # calico-* and coredns Running
```

Every node must be `Ready`, on the same Kubernetes version, with `containerd` as
the runtime:

```
NAME            STATUS   ROLES           VERSION   OS-IMAGE          CONTAINER-RUNTIME
control-plane   Ready    control-plane   v1.35.1   Ubuntu 22.04 LTS  containerd://2.2.4
worker-1        Ready    <none>          v1.35.1   Ubuntu 24.04 LTS  containerd://2.3.1
```

The two nodes do not need to run the same Ubuntu release or containerd version;
only the Kubernetes version must match. Reference versions are in
[14-reference-bkc.md](14-reference-bkc.md).


## Troubleshooting

If the install fails, see
[02-kubernetes-install-troubleshooting.md](02-kubernetes-install-troubleshooting.md)
for:

* proxy and `NO_PROXY` failures, including the API server answering `Forbidden`
* image pull / containerd / registry failures
* Calico rollout failures
* failed `kubeadm init` reset and retry guidance

## If you edit the repo somewhere else

The guide assumes the repo lives on the control-plane host. If you keep it on a
workstation instead, push it across and run everything from there:

```bash
scripts/sync-controller.sh          # rsync into ~/mxl-k8s-qos-lab on the controller
```

Next: [03-cpu-qos.md](03-cpu-qos.md).
