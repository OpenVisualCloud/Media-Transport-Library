#!/usr/bin/env bash
# Prepare one Ubuntu host to be a Kubernetes node: containerd + kubeadm/kubelet/
# kubectl at a pinned version, swap off, kernel modules, sysctls, hostname, and
# /etc/hosts. Idempotent - safe to re-run.
#
# Run AS ROOT ON THE NODE ITSELF:
#   sudo ./install-k8s-node.sh <hostname> <hosts-file-entries...>
#
# On a proxied network it configures the proxy for apt, containerd and the
# cluster's own tools before it needs them, and then proves that an image pull
# works - so a blocked registry fails here, on the node that has the problem,
# instead of hanging 'kubeadm init' minutes later. Environment:
#   LAB_HTTP_PROXY            proxy URL; default: this host's own /etc/environment
#   LAB_HTTPS_PROXY           default: LAB_HTTP_PROXY
#   POD_CIDR, SERVICE_CIDR    excluded from the proxy (defaults below)
#   SKIP_REGISTRY_CHECK=1     skip the pull test (pre-loaded images, mirror)
#
# Example, preparing the control-plane node of a two-node cluster (pass one
# "<address> <node-name>" pair per node in the cluster, including this one):
#   sudo ./install-k8s-node.sh control-plane \
#        "10.0.0.10 control-plane" "10.0.0.11 worker-1"
#
# Normally you do not call this directly: scripts/install-k8s-cluster.sh stages
# and runs it on every node for you.
set -Eeuo pipefail

K8S_VERSION="${K8S_VERSION:-1.35.1-1.1}"
# The apt repository is per minor release, so it has to follow K8S_VERSION:
# overriding the version alone must not leave us pointing at another minor's repo.
K8S_MINOR="${K8S_MINOR:-$(printf '%s' "${K8S_VERSION%%-*}" | cut -d. -f1,2)}"
PAUSE_IMAGE="${PAUSE_IMAGE:-registry.k8s.io/pause:3.10.1}"
POD_CIDR="${POD_CIDR:-10.244.0.0/16}"
SERVICE_CIDR="${SERVICE_CIDR:-10.96.0.0/12}"

[[ $EUID -eq 0 ]] || { echo "FATAL: run as root" >&2; exit 2; }
NODE_HOSTNAME="${1:-}"
[[ -n "$NODE_HOSTNAME" ]] || { echo "usage: $0 <hostname> [\"<ip> <name>\" ...]" >&2; exit 2; }
shift
HOSTS_ENTRIES=("$@")

echo "== 1/8 proxy configuration =="
# This has to come first: apt is used two stages down, and on a proxied network
# apt cannot reach the archives without a proxy setting of its own. Nothing here
# runs at all on a direct connection.
proxy_val() {   # $1 = http | https | no
  local name="$1" v
  # '|| true': grep finding nothing must not fail the pipeline under pipefail.
  v="$( { grep -hiE "^[[:space:]]*${name}_proxy=" /etc/environment 2>/dev/null || true; } | head -1 | cut -d= -f2- | tr -d '"'"'"'')"
  [[ -n "$v" ]] || v="$(printenv "${name}_proxy" 2>/dev/null || true)"
  [[ -n "$v" ]] || v="$(printenv "$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]')_PROXY" 2>/dev/null || true)"
  printf '%s' "$v"
}
# Replace one variable in /etc/environment, keeping both spellings, because
# different programs read different ones.
set_env_var() {   # set_env_var <name> <value>
  local name="$1" value="$2" upper
  upper="$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]')"
  sed -i -E "/^[[:space:]]*($name|$upper)=/d" /etc/environment
  printf '%s="%s"\n%s="%s"\n' "$name" "$value" "$upper" "$value" >>/etc/environment
}
# Proxy exclusion lists are merged, never replaced: whatever this host already
# excluded has to stay excluded.
csv_merge() {   # csv_merge <csv>... -> the union, in order, no duplicates
  local out="" csv tok toks
  for csv in "$@"; do
    [[ -n "$csv" ]] || continue
    IFS=, read -r -a toks <<<"$csv"
    for tok in "${toks[@]}"; do
      [[ -n "$tok" ]] || continue
      case ",$out," in *",$tok,"*) ;; *) out="${out:+$out,}$tok" ;; esac
    done
  done
  printf '%s' "$out"
}
csv_covers() {   # csv_covers <have> <want> -> 0 when every want token is in have
  local have="$1" want="$2" tok toks
  IFS=, read -r -a toks <<<"$want"
  for tok in "${toks[@]}"; do
    [[ -n "$tok" ]] || continue
    case ",$have," in *",$tok,"*) ;; *) return 1 ;; esac
  done
}
# kubeadm and kubectl honour no_proxy from the environment as well, and they talk
# to the API server on the node's own IP. If that IP is not excluded, the proxy
# answers the request - usually 403 - and 'kubeadm init' dies in wait-control-plane
# with "kube-apiserver is not healthy after 4m0s" while kubectl says "Unable to
# connect to the server: Forbidden". Neither mentions a proxy, so make sure the
# host's own no_proxy covers the cluster too.
ensure_env_no_proxy() {
  local want="$1" cur
  cur="$(proxy_val no)"
  if csv_covers "$cur" "$want"; then
    echo "  /etc/environment no_proxy already covers the cluster"
    return
  fi
  set_env_var no_proxy "$(csv_merge "$cur" "$want")"
  echo "  /etc/environment no_proxy now covers the cluster"
}

# NO_PROXY must cover the cluster's own traffic: without the CIDRs and the node
# addresses the cluster comes up and then the API server, kubelet and CNI talk to
# each other through the proxy, which fails in ways that look like CNI bugs.
CLUSTER_NO_PROXY="localhost,127.0.0.1,$SERVICE_CIDR,$POD_CIDR,.svc,.cluster.local"
for entry in "${HOSTS_ENTRIES[@]}"; do CLUSTER_NO_PROXY+=",${entry%% *},${entry##* }"; done

HTTP_P="${LAB_HTTP_PROXY:-$(proxy_val http)}"
HTTPS_P="${LAB_HTTPS_PROXY:-$(proxy_val https)}"
[[ -n "$HTTP_P" ]] || HTTP_P="$HTTPS_P"
[[ -n "$HTTPS_P" ]] || HTTPS_P="$HTTP_P"
APT_PROXY_CONF=/etc/apt/apt.conf.d/95mxl-lab-proxy

if [[ -z "$HTTP_P" ]]; then
  echo "  no proxy configured or found in /etc/environment - assuming direct access"
else
  echo "  proxy: $HTTPS_P"
  # apt reads neither the environment of a sudo'd shell nor /etc/environment
  # reliably, so give it its own file. Ours alone - other apt proxy settings on
  # the host are left in place.
  mkdir -p /etc/apt/apt.conf.d
  WANT_APT_CONF="$(printf 'Acquire::http::Proxy "%s";\nAcquire::https::Proxy "%s";' "$HTTP_P" "$HTTPS_P")"
  if [[ -f "$APT_PROXY_CONF" ]] && [[ "$(cat "$APT_PROXY_CONF")" == "$WANT_APT_CONF" ]]; then
    echo "  apt already uses it ($APT_PROXY_CONF)"
  else
    printf '%s\n' "$WANT_APT_CONF" >"$APT_PROXY_CONF"
    echo "  $APT_PROXY_CONF written (apt needs a proxy setting of its own)"
  fi
  [[ -n "$(proxy_val http)"  ]] || set_env_var http_proxy  "$HTTP_P"
  [[ -n "$(proxy_val https)" ]] || set_env_var https_proxy "$HTTPS_P"
  ensure_env_no_proxy "$CLUSTER_NO_PROXY"
fi

echo "== 2/8 hostname and /etc/hosts =="
hostnamectl set-hostname "$NODE_HOSTNAME"
for entry in "${HOSTS_ENTRIES[@]}"; do
  name="${entry##* }"
  grep -qE "[[:space:]]$name([[:space:]]|\$)" /etc/hosts || printf '%s\n' "$entry" >>/etc/hosts
done

echo "== 3/8 swap off (kubelet refuses to start with swap enabled) =="
swapoff -a
sed -i.bak -E '/[[:space:]]swap[[:space:]]/ s/^([^#])/#\1/' /etc/fstab

echo "== 4/8 kernel modules and sysctls =="
printf 'overlay\nbr_netfilter\n' >/etc/modules-load.d/k8s.conf
modprobe overlay
modprobe br_netfilter
cat >/etc/sysctl.d/k8s.conf <<'EOF'
net.bridge.bridge-nf-call-iptables  = 1
net.bridge.bridge-nf-call-ip6tables = 1
net.ipv4.ip_forward                 = 1
EOF
sysctl --system >/dev/null

echo "== 5/8 containerd with the systemd cgroup driver =="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq ca-certificates curl gnupg
install -m 0755 -d /etc/apt/keyrings
if [[ ! -f /etc/apt/keyrings/docker.asc ]]; then
  curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
  chmod a+r /etc/apt/keyrings/docker.asc
fi
printf 'deb [arch=%s signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu %s stable\n' \
  "$(dpkg --print-architecture)" "$(. /etc/os-release && echo "$VERSION_CODENAME")" \
  >/etc/apt/sources.list.d/docker.list
apt-get update -qq
apt-get install -y -qq containerd.io
mkdir -p /etc/containerd
containerd config default >/etc/containerd/config.toml
# Kubernetes and containerd must agree on the cgroup driver, or the kubelet
# cannot enforce the cpuset that the static CPU Manager assigns.
sed -i 's/SystemdCgroup = false/SystemdCgroup = true/' /etc/containerd/config.toml
sed -i -E "s#(sandbox_image = ).*#\1'$PAUSE_IMAGE'#" /etc/containerd/config.toml
sed -i -E "s#(sandbox = ).*#\1'$PAUSE_IMAGE'#" /etc/containerd/config.toml
systemctl restart containerd
systemctl enable --now containerd >/dev/null 2>&1

# containerd does not inherit the shell's proxy or apt's: it is a system service
# with an environment of its own. Without a drop-in, apt works and every image
# pull times out - which surfaces minutes later as 'kubeadm init' sitting
# silently on "[preflight] Pulling images".
DROPIN_DIR=/etc/systemd/system/containerd.service.d
DROPIN="$DROPIN_DIR/http-proxy.conf"
# Sorts after http-proxy.conf, and systemd lets the last definition of a variable
# win - so this corrects the NO_PROXY of a hand-written drop-in without editing it.
DROPIN_EXTRA="$DROPIN_DIR/zz-mxl-lab-no-proxy.conf"
if [[ -n "$HTTP_P" ]]; then
  dropin_no_proxy() {   # NO_PROXY as the drop-ins currently leave it
    csv_merge "$( { grep -hE '^[[:space:]]*Environment="?(NO_PROXY|no_proxy)=' \
      "$DROPIN" "$DROPIN_EXTRA" 2>/dev/null || true; } \
      | sed -E 's/^[[:space:]]*Environment="?(NO_PROXY|no_proxy)=//; s/"[[:space:]]*$//' \
      | tr '\n' ',')"
  }
  RESTART=0
  mkdir -p "$DROPIN_DIR"
  if [[ ! -f "$DROPIN" ]]; then
    echo "  containerd has no proxy drop-in - writing $DROPIN"
    {
      echo '# Written by install-k8s-node.sh.'
      echo '[Service]'
      printf 'Environment="HTTP_PROXY=%s"\n'  "$HTTP_P"
      printf 'Environment="HTTPS_PROXY=%s"\n' "$HTTPS_P"
      printf 'Environment="NO_PROXY=%s"\n'    "$(csv_merge "$CLUSTER_NO_PROXY" "$(proxy_val no)")"
    } >"$DROPIN"
    RESTART=1
  elif csv_covers "$(dropin_no_proxy)" "$CLUSTER_NO_PROXY"; then
    echo "  containerd's proxy drop-in already covers the cluster - leaving it alone"
  else
    # An existing drop-in is never rewritten. But if it does not exclude the
    # cluster's own addresses, pulls work and the API server becomes unreachable
    # later, so add the exclusions in a file of our own.
    echo "  containerd's proxy drop-in does not exclude the cluster - adding"
    echo "    $DROPIN_EXTRA"
    printf '# Written by install-k8s-node.sh: NO_PROXY for this cluster.\n[Service]\nEnvironment="NO_PROXY=%s"\n' \
      "$(csv_merge "$CLUSTER_NO_PROXY" "$(dropin_no_proxy)" "$(proxy_val no)")" >"$DROPIN_EXTRA"
    RESTART=1
  fi
  if (( RESTART )); then
    systemctl daemon-reload
    systemctl restart containerd
  fi
fi

echo "== 6/8 kubeadm, kubelet, kubectl $K8S_VERSION =="
mkdir -p -m 755 /etc/apt/keyrings
if [[ ! -f /etc/apt/keyrings/kubernetes-apt-keyring.gpg ]]; then
  curl -fsSL "https://pkgs.k8s.io/core:/stable:/v$K8S_MINOR/deb/Release.key" \
    | gpg --dearmor -o /etc/apt/keyrings/kubernetes-apt-keyring.gpg
fi
printf 'deb [signed-by=/etc/apt/keyrings/kubernetes-apt-keyring.gpg] https://pkgs.k8s.io/core:/stable:/v%s/deb/ /\n' \
  "$K8S_MINOR" >/etc/apt/sources.list.d/kubernetes.list
apt-get update -qq
apt-mark unhold kubelet kubeadm kubectl >/dev/null 2>&1 || true
apt-get install -y -qq --allow-downgrades \
  "kubelet=$K8S_VERSION" "kubeadm=$K8S_VERSION" "kubectl=$K8S_VERSION"
# Holding the packages stops an unattended upgrade from changing the version
# under a running measurement campaign.
apt-mark hold kubelet kubeadm kubectl
crictl config --set runtime-endpoint=unix:///run/containerd/containerd.sock
systemctl enable kubelet >/dev/null 2>&1

echo "== 7/8 registry reachability =="
# The single most common way this install fails is a blocked or unproxied
# registry, and the symptom appears far from the cause: 'kubeadm init' prints
# "[preflight] Pulling images" and then nothing for as long as you let it. One
# pull here turns that into an error with a reason, on the node that has it.
if [[ "${SKIP_REGISTRY_CHECK:-0}" == "1" ]]; then
  echo "  skipped (SKIP_REGISTRY_CHECK=1)"
elif timeout 90 crictl pull "$PAUSE_IMAGE" >/dev/null 2>&1; then
  echo "  ok: containerd pulled $PAUSE_IMAGE"
else
  echo "FATAL: containerd on $NODE_HOSTNAME cannot pull $PAUSE_IMAGE." >&2
  echo >&2
  if [[ -n "$HTTP_P" ]]; then
    echo "       This host uses the proxy $HTTPS_P, and containerd's drop-in is" >&2
    echo "       $DROPIN. Check the URL and NO_PROXY in it, then:" >&2
    echo "         systemctl daemon-reload && systemctl restart containerd" >&2
  else
    echo "       No proxy is configured, so containerd is trying to reach the" >&2
    echo "       registry directly. If this network needs a proxy, put it in" >&2
    echo "       LAB_HTTP_PROXY in config/lab.env (or http_proxy in" >&2
    echo "       /etc/environment) and run the installer again - it configures" >&2
    echo "       apt, containerd and kubeadm from that one value." >&2
  fi
  echo >&2
  echo "       The error itself:" >&2
  timeout 90 crictl pull "$PAUSE_IMAGE" 2>&1 | sed 's/^/         /' >&2 || true
  echo >&2
  echo "       Details: \"Behind a proxy\" in docs/02-kubernetes-install.md." >&2
  echo "       Images already on the node (mirror, air-gapped): SKIP_REGISTRY_CHECK=1." >&2
  exit 1
fi
# Calico does not come from registry.k8s.io: the operator image is on quay.io and
# Calico's own images on docker.io. A proxy that allows one registry and blocks
# another is common, and the symptom is stage 3/5 of the cluster installer timing
# out - far from here - so say something now. Advisory: this probes the TLS
# endpoint, which is not quite the same thing as containerd pulling from it.
for reg in quay.io registry-1.docker.io; do
  [[ "${SKIP_REGISTRY_CHECK:-0}" == "1" ]] && break
  code="$(timeout 25 curl -s -o /dev/null -w '%{http_code}' \
    ${HTTPS_P:+--proxy "$HTTPS_P"} "https://$reg/v2/" 2>/dev/null || true)"
  if [[ "$code" == "200" || "$code" == "401" ]]; then
    echo "  ok: $reg answers (Calico's images come from there)"
  else
    echo "  WARN: $reg answered '${code:-nothing}'. Calico's images come from there,"
    echo "        so stage 3/5 of the cluster installer will fail until it is"
    echo "        reachable - see \"Calico never rolls out\" in docs/02-kubernetes-install.md."
  fi
done

echo "== 8/8 lab prerequisites =="
# python3-venv and make are for the case where this node is also the host you
# drive runs from - scripts/setup.sh and the Makefile need them.
apt-get install -y -qq numactl stress-ng util-linux procps dmidecode \
  python3 python3-venv make chrony
systemctl enable --now chrony >/dev/null
chronyc -a makestep >/dev/null
echo "  waiting up to 60s for NTP clock synchronization"
clock_synchronized=0
for _ in {1..30}; do
  if chronyc tracking 2>/dev/null \
      | grep -Eq '^Leap status[[:space:]]*:[[:space:]]*Normal[[:space:]]*$'; then
    clock_synchronized=1
    break
  fi
  sleep 2
done
if [[ "$clock_synchronized" != "1" ]]; then
  echo "FATAL: $NODE_HOSTNAME did not synchronize its clock with NTP." >&2
  echo "       Permit NTP (UDP 123) to the configured time sources, then re-run." >&2
  chronyc sources -v >&2 || true
  chronyc tracking >&2 || true
  exit 1
fi
echo "  ok: clock synchronized"
mkdir -p /opt/mxl-media /dev/shm/mxl

echo
echo "Node $NODE_HOSTNAME prepared: containerd $(containerd --version | awk '{print $3}'), kubeadm $(kubeadm version -o short)"
echo "Next: 'kubeadm init' on the controller, or 'kubeadm join' on a worker."
