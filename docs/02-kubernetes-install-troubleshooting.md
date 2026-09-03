# 2. Kubernetes installation — troubleshooting

This page collects symptoms, likely causes, and fixes for failures during
`scripts/install-k8s-cluster.sh`. Return to
[02-kubernetes-install.md](02-kubernetes-install.md) once the issue is resolved.

> **Troubleshooting first (proxy quick rules):**
> - Put `LAB_HTTP_PROXY` and `LAB_HTTPS_PROXY` in `config/lab.env` when this network
>   requires a proxy.
> - The scripts build cluster `NO_PROXY` from inventory automatically.
> - `kubectl` commands you type manually may still need shell
>   `no_proxy`/`NO_PROXY`.

---

## Proxy and NO_PROXY failures

### Symptom: image pulls time out (dial tcp … i/o timeout)

```
level=info  msg="fetch failed" error="... dial tcp <registry IP>:443: i/o timeout" host=registry.k8s.io
level=error msg="PullImage \"registry.k8s.io/kube-proxy:v1.35.1\" failed" error="... DeadlineExceeded ..."
```

**Cause:** containerd has no proxy configured and is trying to reach the registry
directly.

**Fix:** Set `LAB_HTTP_PROXY` in `config/lab.env` and re-run the installer. Stage 1/5
will write the proxy into containerd's drop-in file before any pull is attempted.

Confirm containerd can reach the registry before waiting for `kubeadm init` to time
out:

```bash
sudo crictl pull registry.k8s.io/pause:3.10.1
```

This should complete in seconds. If it hangs, containerd cannot reach the registry;
fix the proxy first.

To watch pull progress:

```bash
pgrep -a kubeadm                                        # still running?
sudo crictl images | wc -l                             # count grows as pulls land
sudo journalctl -u containerd -n 40 --no-pager         # pull errors and retries
tail -f ~/kubeadm-init.log                             # kubeadm's own output
```

---

### Symptom: API server answers `Forbidden`

```
[control-plane-check] kube-apiserver is not healthy after 4m0.001354689s
error: ... kube-apiserver check failed at https://<control-plane IP>:6443/livez
Unable to connect to the server: Forbidden
```

Or in a later chapter:

```
== 1/4 draining worker-1 ==
Unable to connect to the server: Forbidden
```

**Cause:** The control-plane node's own IP is not in `NO_PROXY`, so `kubectl` and
`kubeadm` send the request to the proxy, which refuses `CONNECT` to port 6443 with
HTTP 403. The control plane itself is usually fine.

`Forbidden` is the tell. A crashed component gives `connection refused`, a TLS error,
or a `CrashLoopBackOff` in `crictl ps -a`, not `Forbidden`.

**Prove it:**

```bash
curl -sk -o /dev/null -w 'direct: %{http_code}\n' --noproxy '*' https://<control-plane IP>:6443/livez
curl -sk -o /dev/null -w 'proxied: %{http_code}\n'              https://<control-plane IP>:6443/livez
```

`direct: 200` with `proxied: 403` confirms this problem.

**Fix — on each node:**

```bash
sudo tee -a /etc/environment >/dev/null <<'EOF'
no_proxy="localhost,127.0.0.1,10.96.0.0/12,10.244.0.0/16,<control-plane IP>,<worker IP>,control-plane,worker-1,.svc,.cluster.local"
NO_PROXY="localhost,127.0.0.1,10.96.0.0/12,10.244.0.0/16,<control-plane IP>,<worker IP>,control-plane,worker-1,.svc,.cluster.local"
EOF
```

Log out and back in for it to take effect on your own shell.

**Fix — in your local shell** (for `kubectl` you type by hand):

```bash
cat >>~/.bashrc <<'EOF'
export no_proxy="localhost,127.0.0.1,10.96.0.0/12,10.244.0.0/16,<control-plane IP>,<worker IP>,control-plane,worker-1,.svc,.cluster.local"
export NO_PROXY="$no_proxy"
EOF
```

`localhost` and `127.0.0.1` are not decoration: `scripts/port-forward-prometheus.sh`
publishes Prometheus on `127.0.0.1:19090`, and without them the runner asks the
proxy for it and every measurement fails with nothing wrong in the cluster.

`scripts/preflight.sh` reports the gap:

```
  WARN   proxy set, and no_proxy in this shell does not cover the cluster
```

The permanent fix is `LAB_HTTP_PROXY` in `config/lab.env` — stage 1/5 then writes
`NO_PROXY` everywhere automatically, including the union of any existing containerd
drop-in you already have.

---

### kubeadm `HTTPProxy` warnings

```
[WARNING HTTPProxy]: Connection to "https://<control-plane IP>" uses proxy "<proxy>"
[WARNING HTTPProxyCIDR]: connection to "10.96.0.0/12" uses proxy "<proxy>"
```

These warn that the node would talk to itself or to cluster-internal traffic through
the proxy. Both mean something is missing from `NO_PROXY`. The installer composes and
writes `NO_PROXY` from `config/nodes.env` automatically; if you see these warnings the
node was prepared before the current installer was in place. Apply the fix above.

---

## Image pull / containerd / registry failures

### Symptom: pull fails at stage 1/5

```
FATAL: containerd on worker-1 cannot pull registry.k8s.io/pause:3.10.1.

       No proxy is configured, so containerd is trying to reach the
       registry directly. If this network needs a proxy, put it in
       LAB_HTTP_PROXY in config/lab.env ...
```

**Cause:** stage 1/5 runs a registry reachability check before proceeding; this
failure stops the install before stage 2/5 can hang.

**Fix:** Set `LAB_HTTP_PROXY` and re-run. If you are behind a proxy, the installer
will write the proxy drop-in for containerd and re-test the pull.

If images are already on the node (registry mirror or pre-loaded air-gap set), skip
the pull test:

```bash
SKIP_REGISTRY_CHECK=1 scripts/install-k8s-cluster.sh
```

---

### Manual proxy check for containerd

If you need to configure containerd by hand (e.g., to check what the installer wrote):

```bash
sudo mkdir -p /etc/systemd/system/containerd.service.d
sudo tee /etc/systemd/system/containerd.service.d/http-proxy.conf >/dev/null <<'EOF'
[Service]
Environment="HTTP_PROXY=<proxy URL>"
Environment="HTTPS_PROXY=<proxy URL>"
Environment="NO_PROXY=localhost,127.0.0.1,10.96.0.0/12,10.244.0.0/16,<control-plane IP>,<worker IP>,control-plane,worker-1,.svc,.cluster.local"
EOF
sudo systemctl daemon-reload && sudo systemctl restart containerd
```

Verify it took effect:

```bash
sudo systemctl show containerd --property=Environment   # must list HTTPS_PROXY
sudo crictl pull registry.k8s.io/pause:3.10.1           # seconds, not a 30 s timeout
```

Give `apt` its own setting too — it reads neither your shell nor `/etc/environment`:

```bash
sudo tee /etc/apt/apt.conf.d/95mxl-lab-proxy >/dev/null <<'EOF'
Acquire::http::Proxy "<proxy URL>";
Acquire::https::Proxy "<proxy URL>";
EOF
```

---

## Calico rollout failures

### Symptom: tigera-operator times out

```
Waiting for deployment "tigera-operator" rollout to finish: 0 out of 1 new replicas have been updated...
error: timed out waiting for the condition
```

**Cause:** the operator pod never became available. The most common reason is
`ImagePullBackOff` — Calico's images come from `quay.io/tigera/operator` and
`docker.io/calico`, not `registry.k8s.io`. A proxy that allows the Kubernetes
registry can still refuse those.

The installer prints pod details before it exits; to check by hand:

```bash
kubectl -n tigera-operator get pods -o wide
kubectl -n tigera-operator describe pod -l k8s-app=tigera-operator | tail -40
kubectl -n tigera-operator get events --sort-by=.lastTimestamp | tail -20
```

Stage 1/5 checks those registries and warns if they are unreachable:

```
  WARN: quay.io answered 'nothing'. Calico's images come from there, so stage 3/5
        of the cluster installer will fail until it is reachable
```

**Fix:** Test the exact image named in the event:

```bash
sudo crictl pull quay.io/tigera/operator:v1.42.0
```

If this fails, get `quay.io` and `docker.io` allowed through your proxy, or point
`CALICO_OPERATOR_URL` at a mirror you can reach.

**Two things it is not:**

- *The missing CNI.* The operator runs with `hostNetwork: true`; it does not need
  pod networking to start.
- *The control-plane taint.* Its Deployment tolerates every `NoSchedule` and
  `NoExecute` taint, so a single-node cluster schedules it fine.

Re-running `scripts/install-k8s-cluster.sh` after fixing the cause is safe; stage
3/5 reconciles what is already there.

---

### Symptom: `AlreadyExists` from an older checkout

```
Error from server (AlreadyExists): error when creating "...tigera-operator.yaml":
  namespaces "tigera-operator" already exists
```

**Cause:** the installer was using `kubectl create`, which cannot re-run over its own
work.

**Fix:** Update your checkout — the installer now uses server-side apply and converges
on re-runs. Nothing on the cluster needs cleaning up first.

---

## Failed `kubeadm init` — reset and retry

### Symptom: installer detects a partial `kubeadm init`

```
FATAL: /etc/kubernetes/admin.conf already exists but the cluster is not healthy.
       Reset the control plane before re-running.
```

**Cause:** A previous `kubeadm init` failed in `wait-control-plane` and left
certificates, static Pod manifests, and `/etc/kubernetes/admin.conf` behind.
Re-running cannot repair that state.

**Fix:** On the control-plane host:

```bash
sudo kubeadm reset -f
sudo rm -rf /etc/cni/net.d ~/.kube
```

This removes the partial cluster state and nothing else. Then re-run:

```bash
scripts/install-k8s-cluster.sh
```

---

### Manual node preparation

Stage 1/5 is the only part of the installer that touches each host individually.
If you want to prepare a worker directly on that machine, copy
`scripts/install-k8s-node.sh` there and run it as root with one `"<IP> <node-name>"`
pair per node in the cluster:

```bash
sudo ./install-k8s-node.sh worker-1 "<ctrl IP> control-plane" "<worker IP> worker-1"
```

`scripts/install-k8s-cluster.sh` is then idempotent from stage 2/5 onward and
picks up where the manual step left off.

---

Back to [02-kubernetes-install.md](02-kubernetes-install.md).
