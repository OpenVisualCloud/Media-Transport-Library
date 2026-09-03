#!/usr/bin/env bash
# Automated Kubernetes installer.
#
# Builds the whole cluster from bare Ubuntu hosts: prepares every node, runs
# kubeadm init on the controller, installs Calico, and joins the workers.
# Idempotent - re-running skips whatever is already in place.
#
# Run ON THE CONTROLLER (any host with SSH key access to every node works, the
# controller included - it is reached the same way as the others):
#   scripts/install-k8s-cluster.sh
#
# Inventory comes from config/nodes.env (LAB_CONTROLLER and LAB_WORKERS in
# config/lab.env decide which entries are the controller and the workers).
# Each node's sudo password is typed into that node's own prompt.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/config/lab.env"
# shellcheck source=/dev/null
source "$ROOT/config/nodes.env"
: "${LAB_SSH_USER:?set LAB_SSH_USER in config/nodes.env}"

POD_CIDR="${POD_CIDR:-10.244.0.0/16}"
SERVICE_CIDR="${SERVICE_CIDR:-10.96.0.0/12}"
K8S_VERSION="${K8S_VERSION:-1.35.1-1.1}"
# Proxy, if this network has one. Order: config/lab.env, then this shell's own
# environment, then whatever each node already has in /etc/environment. Empty
# everywhere means a direct connection, which is the normal case.
LAB_HTTP_PROXY="${LAB_HTTP_PROXY:-${http_proxy:-${HTTP_PROXY:-}}}"
LAB_HTTPS_PROXY="${LAB_HTTPS_PROXY:-${https_proxy:-${HTTPS_PROXY:-$LAB_HTTP_PROXY}}}"
CALICO_OPERATOR_URL="${CALICO_OPERATOR_URL:-https://raw.githubusercontent.com/projectcalico/calico/v3.32.0/manifests/tigera-operator.yaml}"
SSH_USER="${LAB_SSH_USER}"

# A root login needs no sudo - and then nothing here has to prompt, so every
# remote command can run over BatchMode SSH and the whole install is
# unattended. A normal login gets 'sudo' plus a tty, so that the password
# prompt appears on the node that is asking for it.
# shellcheck source=lib/remote-admin.sh
source "$ROOT/scripts/lib/remote-admin.sh"
lab_remote_admin_init "$SSH_USER"
SUDO="$LAB_REMOTE_SUDO"
SSH_ADMIN=("${LAB_REMOTE_SSH[@]}")

host_of() { local key="${1^^}_HOST"; key="${key//-/_}"; printf '%s' "${!key:-}"; }

# SSH is happy with a ~/.ssh/config alias or a DNS name, but /etc/hosts and
# kubeadm's --apiserver-advertise-address need a real IP that the other nodes can
# reach. Resolve it here so a wrong value fails now, with an explanation, instead
# of half way through kubeadm init.
ip_of() {
  local node="$1" addr ip
  addr="$(host_of "$node")"
  [[ -n "$addr" ]] || { echo "FATAL: no address for $node in config/nodes.env" >&2; exit 2; }
  if [[ "$addr" =~ ^[0-9]+(\.[0-9]+){3}$ ]]; then
    ip="$addr"
  else
    ip="$(getent ahostsv4 "$addr" 2>/dev/null | awk 'NR==1{print $1}')"
    [[ -n "$ip" ]] || {
      echo "FATAL: cannot resolve '$addr' to an IP address for node $node." >&2
      echo "       Kubernetes needs an address every node can reach, so put the" >&2
      echo "       node's IP in config/nodes.env - a ~/.ssh/config alias is not" >&2
      echo "       enough for the installer." >&2
      exit 2
    }
  fi
  case "$ip" in
    127.*|0.0.0.0)
      echo "FATAL: node $node is configured as '$addr' ($ip), a loopback address." >&2
      echo "       Even when you run this on the control-plane host, every node has" >&2
      echo "       to be reachable from the others, so use its real IP." >&2
      exit 2 ;;
  esac
  printf '%s' "$ip"
}

CONTROLLER="$LAB_CONTROLLER"
IFS=, read -r -a WORKERS <<<"$LAB_WORKERS"
ALL_NODES=("$CONTROLLER" "${WORKERS[@]}")

# /etc/hosts entries: every node must resolve every other node by Kubernetes
# node name, because that is the name kubeadm puts in certificates and joins.
HOSTS_ENTRIES=()
for node in "${ALL_NODES[@]}"; do
  HOSTS_ENTRIES+=("$(ip_of "$node") $node")
done

elapsed() { printf '%dm%02ds' $((SECONDS/60)) $((SECONDS%60)); }
step()    { printf '\n== %s  (t+%s) ==\n' "$1" "$(elapsed)"; }

# 'set -e' on its own exits without a word, so the last thing on screen is
# whatever the failing command printed - which does not say that the installer
# gave up, or that re-running is safe. Say both.
on_error() {
  local rc=$? line="$1"
  echo >&2
  echo "FATAL: the installer stopped at line $line (exit $rc) after $(elapsed)." >&2
  echo "       The output just above this is the reason." >&2
  echo "       Nothing needs undoing: this script is idempotent, so fix the cause" >&2
  echo "       and run it again - it skips whatever is already in place." >&2
  exit "$rc"
}
trap 'on_error $LINENO' ERR

# Some steps here print nothing at all while they work - image pulls, Calico's
# rollout, 'kubectl wait'. Silence is indistinguishable from a hang, so tick
# while the quiet ones run instead of leaving the screen dead.
with_progress() {   # with_progress "<what is being waited for>" cmd...
  local label="$1"; shift
  "$@" </dev/null &
  local pid=$! waited=0 rc=0
  while kill -0 "$pid" 2>/dev/null; do
    sleep 15; waited=$((waited+15))
    kill -0 "$pid" 2>/dev/null && printf '   ... %s, %ds elapsed\n' "$label" "$waited"
  done
  wait "$pid" || rc=$?
  return $rc
}

# A rollout or a 'kubectl wait' that times out says only "timed out waiting for the
# condition" - never why. The cluster knows why, so ask it before giving up.
diagnose() {   # diagnose "<what timed out>"
  echo >&2
  echo "FATAL: $1" >&2
  echo >&2
  echo "-- pods (a pod stuck Pending or ImagePullBackOff is the usual reason):" >&2
  ssh -o BatchMode=yes "$CTRL" "$NP kubectl get pods -A -o wide" >&2 || true
  echo >&2
  echo "-- the last 25 events:" >&2
  ssh -o BatchMode=yes "$CTRL" "$NP kubectl get events -A --sort-by=.lastTimestamp 2>/dev/null | tail -25" >&2 || true
  echo >&2
  echo "       ImagePullBackOff on a quay.io image: Calico does not come from" >&2
  echo "       registry.k8s.io, so a proxy that allows one can still block the" >&2
  echo "       other. Test the exact image the event names, on $CONTROLLER:" >&2
  echo "         sudo crictl pull <image from the event above>" >&2
  echo "       Pending with 'untolerated taint' or 'cni plugin not initialized':" >&2
  echo "       see \"Calico never rolls out\" in docs/02-kubernetes-install.md." >&2
  echo >&2
  echo "       Nothing needs undoing before you re-run this script: it is" >&2
  echo "       idempotent and picks up where it stopped." >&2
  exit 1
}

# kubeadm and kubectl are Go programs: they honour http_proxy/https_proxy from the
# environment, so on a proxied host they try to reach the API server *through* the
# proxy. A proxy typically refuses CONNECT to :6443 with 403, which kubeadm reports
# as "kube-apiserver is not healthy after 4m0s" and kubectl as "Unable to connect to
# the server: Forbidden" - neither of which mentions the proxy. So every remote
# command below carries an explicit no_proxy covering the cluster's own addresses.
#
# scripts/lib/no-proxy.sh composes that list, and every other script in this repo
# uses the same one for its own kubectl - so there is one definition of what the
# cluster's own traffic is.
# shellcheck source=lib/no-proxy.sh
source "$ROOT/scripts/lib/no-proxy.sh"
NO_PROXY_EXTRA=()   # HOSTS_ENTRIES is "<ip> <node>"; the helper wants csv tokens
for entry in "${HOSTS_ENTRIES[@]}"; do NO_PROXY_EXTRA+=("${entry%% *},${entry##* }"); done
CLUSTER_NO_PROXY="$(lab_cluster_no_proxy "${NO_PROXY_EXTRA[@]}")"
# `env` runs under sudo when needed, so the root kubeadm process gets NO_PROXY.
NP="env no_proxy='$CLUSTER_NO_PROXY' NO_PROXY='$CLUSTER_NO_PROXY'"
lab_export_no_proxy   # and for whatever this script runs locally

echo "== plan =="
echo "  controller: $CONTROLLER ($(host_of "$CONTROLLER"))"
for node in "${WORKERS[@]}"; do echo "  worker:     $node ($(host_of "$node"))"; done
echo "  kubernetes: $K8S_VERSION    pod CIDR: $POD_CIDR    CNI: Calico (VXLAN)"
echo "  login:      $SSH_USER${SUDO:+ (sudo password prompted per node)}"
if [[ -n "$LAB_HTTP_PROXY$LAB_HTTPS_PROXY" ]]; then
  echo "  proxy:      ${LAB_HTTPS_PROXY:-$LAB_HTTP_PROXY} - apt, containerd and kubeadm get it on every node"
else
  echo "  proxy:      none (set LAB_HTTP_PROXY in config/lab.env if this network needs one)"
fi
echo "  expect 10-20 minutes; each stage below is timestamped with t+"

# ---------------------------------------------------------------------------
step "1/5 preparing every node (containerd, kubeadm, swap, sysctls)"
n=0
for node in "${ALL_NODES[@]}"; do
  addr="$(host_of "$node")"
  n=$((n+1))
  echo "-- $node ($n of ${#ALL_NODES[@]}), 2-6 min: apt output follows"
  scp -q "$ROOT/scripts/install-k8s-node.sh" "$SSH_USER@$addr:/tmp/install-k8s-node.sh"
  # POD_CIDR/SERVICE_CIDR are for the containerd proxy NO_PROXY the node script
  # writes on a proxied host: cluster-internal traffic must not go via a proxy.
  "${SSH_ADMIN[@]}" "$SSH_USER@$addr" \
    "chmod +x /tmp/install-k8s-node.sh && $SUDO K8S_VERSION='$K8S_VERSION' POD_CIDR='$POD_CIDR' SERVICE_CIDR='$SERVICE_CIDR' SKIP_REGISTRY_CHECK='${SKIP_REGISTRY_CHECK:-0}' LAB_HTTP_PROXY='$LAB_HTTP_PROXY' LAB_HTTPS_PROXY='$LAB_HTTPS_PROXY' /tmp/install-k8s-node.sh $node $(printf "'%s' " "${HOSTS_ENTRIES[@]}")"
done

# ---------------------------------------------------------------------------
CTRL_ADDR="$(host_of "$CONTROLLER")"
CTRL="$SSH_USER@$CTRL_ADDR"
CTRL_IP="$(ip_of "$CONTROLLER")"      # what the API server advertises to the workers

step "2/5 control plane on $CONTROLLER"
# admin.conf alone does not mean the control plane finished: kubeadm writes it
# before it waits for the API server, so a run that died in wait-control-plane
# leaves the file behind. Ask the cluster instead, and refuse to build on a
# half-initialised one - re-running kubeadm init cannot repair it.
INIT_DONE=0
if ssh -o BatchMode=yes "$CTRL" 'test -f /etc/kubernetes/admin.conf'; then
  echo "-- /etc/kubernetes/admin.conf exists; checking the control plane really finished"
  if "${SSH_ADMIN[@]}" "$CTRL" "$SUDO $NP kubectl --kubeconfig=/etc/kubernetes/admin.conf -n kube-system get configmap kubeadm-config >/dev/null 2>&1"; then
    echo "  already initialised - skipping kubeadm init"
    INIT_DONE=1
  else
    echo "FATAL: 'kubeadm init' has run on $CONTROLLER but did not finish - the API" >&2
    echo "       server is not serving a usable cluster. Most often it failed in" >&2
    echo "       'wait-control-plane' because a proxy answered the health check:" >&2
    echo "         kube-apiserver is not healthy after 4m0s" >&2
    echo "         Unable to connect to the server: Forbidden" >&2
    echo "       See \"The API server answers 'Forbidden'\" in" >&2
    echo "       docs/02-kubernetes-install.md - the fix is no_proxy, not a retry." >&2
    echo >&2
    echo "       Then clear the half-finished control plane on $CONTROLLER and" >&2
    echo "       re-run this script. On that host (it removes the cluster's state," >&2
    echo "       manifests and certificates - there is nothing else in it yet):" >&2
    echo "         sudo kubeadm reset -f" >&2
    echo "         sudo rm -rf /etc/cni/net.d ~/.kube" >&2
    exit 1
  fi
fi
if (( ! INIT_DONE )); then
  # Pull the control-plane images explicitly first. kubeadm's own preflight pull
  # prints "[preflight] Pulling images" and then nothing for several minutes;
  # this prints one line per image, so progress - or a stall - is visible.
  echo "-- pulling control-plane images (~700 MB, 1-5 min, one line per image)"
  "${SSH_ADMIN[@]}" "$CTRL" "$SUDO kubeadm config images pull --kubernetes-version=v${K8S_VERSION%%-*}"
  echo "-- kubeadm init (1-3 min once the images are local)"
  "${SSH_ADMIN[@]}" "$CTRL" "
    set -e
    $SUDO $NP kubeadm init \
      --kubernetes-version=v${K8S_VERSION%%-*} \
      --pod-network-cidr='$POD_CIDR' \
      --service-cidr='$SERVICE_CIDR' \
      --apiserver-advertise-address='$CTRL_IP' \
      --node-name='$CONTROLLER' | tee ~/kubeadm-init.log
  "
fi
# kubectl for the unprivileged lab user: everything else in this repo assumes
# plain 'kubectl' works as \$LAB_SSH_USER on the controller.
ssh -o BatchMode=yes "$CTRL" "
  set -e
  mkdir -p ~/.kube
  ${SUDO:+sudo -n} cp -f /etc/kubernetes/admin.conf ~/.kube/config 2>/dev/null || true
  test -f ~/.kube/config
" || "${SSH_ADMIN[@]}" "$CTRL" "mkdir -p ~/.kube && $SUDO cp -f /etc/kubernetes/admin.conf ~/.kube/config && $SUDO chown \$(id -u):\$(id -g) ~/.kube/config"
ssh -o BatchMode=yes "$CTRL" "$NP kubectl get nodes"

# ---------------------------------------------------------------------------
step "3/5 Calico CNI"
# Apply, never create. 'kubectl create -f' fails with AlreadyExists on every
# object a previous run got as far as making, so a stage 3/5 that stopped half way
# used to make the *next* run fail too. Apply converges instead - server-side,
# because tigera-operator.yaml is much larger than the 262144-byte annotation
# that client-side apply would try to store on each object.
if ssh -o BatchMode=yes "$CTRL" "$NP kubectl -n tigera-operator get deployment tigera-operator" >/dev/null 2>&1; then
  echo "-- tigera-operator already created - reconciling it"
else
  echo "-- installing the tigera-operator (it is what installs Calico itself)"
fi
ssh -o BatchMode=yes "$CTRL" "$NP kubectl apply --server-side --force-conflicts -f '$CALICO_OPERATOR_URL'"
with_progress "tigera-operator rolling out (timeout 5m)" \
  ssh -o BatchMode=yes "$CTRL" "$NP kubectl -n tigera-operator rollout status deployment/tigera-operator --timeout=5m" \
  || diagnose "the tigera-operator deployment never became available."
# The CRD comes from the manifest above, and the Installation below is an instance
# of it: applying it the moment the operator is up can lose that race.
ssh -o BatchMode=yes "$CTRL" \
  "$NP kubectl wait --for=condition=established --timeout=60s crd/installations.operator.tigera.io" >/dev/null
scp -q "$ROOT/cluster/calico-installation.yaml" "$CTRL:/tmp/calico-installation.yaml"
ssh -o BatchMode=yes "$CTRL" "$NP kubectl apply --server-side --force-conflicts -f /tmp/calico-installation.yaml"
with_progress "waiting for the control plane to go Ready (timeout 10m)" \
  ssh -o BatchMode=yes "$CTRL" "$NP kubectl wait --for=condition=Ready node --all --timeout=10m" \
  || diagnose "the control-plane node did not go Ready - Calico's own pods are the place to look."

# ---------------------------------------------------------------------------
step "4/5 joining workers"
# Minting the join token is the only place in this installer that needs both a
# root shell on the control plane and a value brought back here, and those two
# needs conflict. 'ssh -t' is what lets sudo prompt for a password, but a pty
# folds the remote /dev/tty and stderr into the ssh client's *stdout*, so
# "$(ssh -t ...)" captures the prompt instead of showing it: the installer sits
# on a blank screen waiting for a password nobody can see. So never capture an
# interactive session. Park the output in a file on the controller and fetch it
# over a second, TTY-less connection.
#
# The privileged step also needs --kubeconfig. kubeadm's help text names
# /etc/kubernetes/admin.conf, but its real default is empty: it falls back to
# $KUBECONFIG and then $HOME/.kube/config, and under sudo $HOME is root's, which
# has no kubeconfig. Without it the command fails on a machine where everything
# is fine.
JOIN_FILE="/tmp/kubeadm-join.$$"
KUBEADM_JOIN="$NP kubeadm token create --print-join-command --kubeconfig=/etc/kubernetes/admin.conf"
# umask first: that file holds a bootstrap token until the fetch below deletes it.
if ssh -o BatchMode=yes "$CTRL" "umask 077; ${SUDO:+sudo -n }$KUBEADM_JOIN >'$JOIN_FILE'" 2>/dev/null; then
  :   # passwordless sudo (or a root login): nothing to prompt for
else
  # Not a failure yet - the likely cause is that sudo wants a password, which
  # needs the interactive connection. Its stderr stays on the screen on purpose.
  [[ -z "$SUDO" ]] || echo "-- enter the sudo password for $CTRL to mint a join token"
  "${SSH_ADMIN[@]}" "$CTRL" "umask 077; $SUDO $KUBEADM_JOIN >'$JOIN_FILE'" || true
fi
JOIN_CMD="$(ssh -o BatchMode=yes "$CTRL" "cat '$JOIN_FILE' 2>/dev/null; rm -f '$JOIN_FILE'" \
  | tr -d '\r' | grep -m1 '^kubeadm join' || true)"
if [[ -z "$JOIN_CMD" ]]; then
  echo >&2
  echo "FATAL: could not obtain a join command from $CONTROLLER after $(elapsed)." >&2
  echo "       The control plane is up - only the token is missing, so nothing is" >&2
  echo "       half-joined and re-running this script is safe." >&2
  echo >&2
  echo "       Any error text above this line is kubeadm's own. If there is none," >&2
  echo "       the sudo authentication on $CONTROLLER did not complete. Run the" >&2
  echo "       command by hand there and watch what it asks for:" >&2
  echo "         ${SUDO:+sudo }kubeadm token create --print-join-command \\" >&2
  echo "           --kubeconfig=/etc/kubernetes/admin.conf" >&2
  echo "       A password prompt means this login needs one; consider a NOPASSWD" >&2
  echo "       sudoers rule for the lab user, which also makes every other script" >&2
  echo "       here non-interactive." >&2
  exit 1
fi
for node in "${WORKERS[@]}"; do
  addr="$(host_of "$node")"
  if ssh -o BatchMode=yes "$CTRL" "$NP kubectl get node '$node'" >/dev/null 2>&1; then
    echo "-- $node already joined"
    continue
  fi
  echo "-- joining $node (1-2 min)"
  "${SSH_ADMIN[@]}" "$SSH_USER@$addr" "$SUDO $NP $JOIN_CMD --node-name='$node'"
done

# ---------------------------------------------------------------------------
step "5/5 cluster status"
with_progress "waiting for every node to go Ready (timeout 10m)" \
  ssh -o BatchMode=yes "$CTRL" "$NP kubectl wait --for=condition=Ready node --all --timeout=10m" \
  || diagnose "not every node went Ready after joining."
ssh -o BatchMode=yes "$CTRL" "$NP kubectl get nodes -o wide"
echo
echo "Total time: $(elapsed)."

cat <<EOF

Cluster is up. Next steps, in order:
  scripts/configure-cpu-qos.sh          # static CPU Manager + single-numa-node
  scripts/bootstrap-worker.sh           # PCM exporter, RDT helper, stress-ng
  scripts/install-observability.sh      # Prometheus, Grafana, node-exporter
  scripts/build-ffmpeg-mxl-image.sh     # workload image on the worker
  scripts/stage-media.sh <clip>...      # source clips
  scripts/preflight.sh                  # verify everything before measuring
EOF
