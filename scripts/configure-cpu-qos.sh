#!/usr/bin/env bash
# CPU QoS configuration package.
#
# Applies the verified kubelet CPU QoS settings to a worker and verifies them
# through the API server. Without these settings a Guaranteed Pod still gets its
# CPU *quota*, but not exclusive whole cores on one NUMA node - so the `pinned`
# scenario collapses onto `baseline` and the density result is meaningless.
#
#   cpuManagerPolicy: static          integer-CPU Guaranteed containers get
#                                     exclusive CPUs instead of a shared quota
#   full-pcpus-only: true             allocate whole physical cores only
#   strict-cpu-reservation: true      keep everything else off reservedSystemCPUs
#   reservedSystemCPUs: 0-3           kubelet, containerd, PCM exporter live here
#   topologyManagerPolicy:            admit a container only if its CPUs and
#     single-numa-node                memory come from one NUMA node
#   topologyManagerScope: container   align per container, not per Pod, so the
#                                     decoder and encoder can sit on one socket
#
# Run FROM THE CONTROLLER:
#   scripts/configure-cpu-qos.sh [NODE]          # default: LAB_DEFAULT_NODE
#   scripts/configure-cpu-qos.sh --verify [NODE] # read back only, change nothing
#
# The worker sudo password is typed into the worker's own prompt; this script
# never stores or forwards it. Applying restarts the kubelet, which briefly
# disrupts Pods on that node - do not run it during a measurement.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/config/lab.env"
# shellcheck source=/dev/null
source "$ROOT/config/nodes.env"
: "${LAB_SSH_USER:?set LAB_SSH_USER in config/nodes.env}"
# shellcheck source=lib/remote-admin.sh
source "$ROOT/scripts/lib/remote-admin.sh"
lab_remote_admin_init "$LAB_SSH_USER"
# shellcheck source=lib/cpu-topology.sh
source "$ROOT/scripts/lib/cpu-topology.sh"
# shellcheck source=lib/no-proxy.sh
source "$ROOT/scripts/lib/no-proxy.sh"
lab_export_no_proxy   # or kubectl below reaches the API server through a proxy

VERIFY_ONLY=0
if [[ "${1:-}" == "--verify" ]]; then VERIFY_ONLY=1; shift; fi
NODE="${1:-$LAB_DEFAULT_NODE}"
KEY="${NODE^^}_HOST"; KEY="${KEY//-/_}"
HOST="${!KEY:-}"
[[ -n "$HOST" ]] || { echo "FATAL: no address for $NODE ($KEY) in config/nodes.env" >&2; exit 2; }
TARGET="${LAB_SSH_USER}@$HOST"
RESERVED="${LAB_RESERVED_CPUS:-0-3}"

# SMT is supported, but config/lab.env has to know about it: DEC_CORES and
# ENC_CORES are physical cores, and the CPU request the kubelet admits is
# cores x LAB_THREADS_PER_CORE. Get that number wrong and full-pcpus-only
# rejects every workload Pod with SMTAlignmentError, or - worse - admits Pods
# that own half the cores the scenario claims and reports a bogus density.
THREADS_PER_CORE="$("${LAB_REMOTE_SSH_CAPTURE[@]}" "$TARGET" "lscpu | awk -F: '/^Thread\\(s\\) per core/{gsub(/ /,\"\",\$2); print \$2}'")"
THREADS_PER_CORE="${THREADS_PER_CORE//[$'\r\n']/}"
CONFIGURED_THREADS="${LAB_THREADS_PER_CORE:-1}"
if [[ -z "$THREADS_PER_CORE" ]]; then
  echo "WARN:  could not read threads per core from $NODE; assuming the configured" >&2
  echo "       LAB_THREADS_PER_CORE=$CONFIGURED_THREADS is right." >&2
elif [[ "$THREADS_PER_CORE" != "$CONFIGURED_THREADS" ]]; then
  echo "FATAL: $NODE exposes $THREADS_PER_CORE hardware thread(s) per physical core," >&2
  echo "       but config/lab.env says LAB_THREADS_PER_CORE=$CONFIGURED_THREADS." >&2
  echo "       Set LAB_THREADS_PER_CORE=$THREADS_PER_CORE and re-check LAB_RESERVED_CPUS:" >&2
  echo "       with SMT on it must cover whole cores, and the sibling of CPU N is" >&2
  echo "       N + sockets x cores_per_socket. Then re-run this script." >&2
  echo "       Alternatively disable Logical Processor in BIOS (or run" >&2
  echo "       scripts/configure-smt-off.sh $NODE), reboot, and run scripts/check-bios.sh." >&2
  exit 2
fi
if [[ "$THREADS_PER_CORE" != "1" ]]; then
  echo "info:  SMT is enabled ($THREADS_PER_CORE threads/core). Guaranteed CPU requests"
  echo "       are DEC_CORES/ENC_CORES x $THREADS_PER_CORE, which is what full-pcpus-only admits."
  # reservedSystemCPUs is about to be written from RESERVED, so refuse a value
  # that would strand the free sibling of every reserved thread.
  CORE_MAP="$("${LAB_REMOTE_SSH_CAPTURE[@]}" "$TARGET" "lscpu -p=CPU,CORE" || true)"
  PARTIAL="$(lab_partially_reserved_cores "$RESERVED" "$CORE_MAP" | tr '\n' ' ')"
  if [[ -z "$CORE_MAP" ]]; then
    echo "WARN:  could not read the CPU-to-core map from $NODE; not checking that" >&2
    echo "       LAB_RESERVED_CPUS=$RESERVED covers whole cores." >&2
  elif [[ -n "${PARTIAL// /}" ]]; then
    echo "FATAL: LAB_RESERVED_CPUS=$RESERVED reserves only part of core(s) ${PARTIAL% }." >&2
    echo "       strict-cpu-reservation keeps the workload off the reserved thread and" >&2
    echo "       full-pcpus-only refuses a core whose sibling is not free, so each of" >&2
    echo "       those cores is lost entirely. Reserve both siblings of every reserved" >&2
    echo "       CPU - 'lscpu -p=CPU,CORE' on $NODE lists the pairs - then re-run." >&2
    exit 2
  fi
fi

# Everything here goes through the API server, so check that once, up front. The
# alternative is a bare kubectl error in the middle of stage 1/4 that names
# neither the cause nor the chapter to go back to.
if ! CLUSTER_INFO="$(kubectl cluster-info 2>&1)"; then
  echo "FATAL: cannot reach the API server. kubectl says:" >&2
  printf '%s\n' "$CLUSTER_INFO" | sed 's/^/         /' >&2
  echo "       'Forbidden' means an HTTP proxy answered instead of the API server:" >&2
  echo "       see \"The API server answers 'Forbidden'\" in docs/02-kubernetes-install.md." >&2
  echo "       Anything else usually means the cluster is not up, or ~/.kube/config is" >&2
  echo "       missing - finish docs/02-kubernetes-install.md first." >&2
  exit 2
fi

verify() {
  echo "== live kubelet configuration on $NODE (read through the API server) =="
  local cfg
  cfg="$(kubectl get --raw "/api/v1/nodes/$NODE/proxy/configz")"
  python3 - "$cfg" <<'PY'
import json, sys
k = json.loads(sys.argv[1])["kubeletconfig"]
expected = {
    "cpuManagerPolicy": "static",
    "topologyManagerPolicy": "single-numa-node",
    "topologyManagerScope": "container",
    "cgroupDriver": "systemd",
}
bad = 0
for key, want in expected.items():
    got = k.get(key)
    flag = "OK   " if got == want else "WRONG"
    if got != want:
        bad = 1
    print(f"  {flag} {key:26} {got}  (expected {want})")
opts = k.get("cpuManagerPolicyOptions") or {}
for key in ("full-pcpus-only", "strict-cpu-reservation"):
    got = opts.get(key)
    flag = "OK   " if got == "true" else "WRONG"
    if got != "true":
        bad = 1
    print(f"  {flag} {key:26} {got}  (expected true)")
reserved = k.get("reservedSystemCPUs")
print(f"  {'OK   ' if reserved else 'WRONG'} {'reservedSystemCPUs':26} {reserved}")
if not reserved:
    bad = 1
print(f"  info  cpuManagerReconcilePeriod  {k.get('cpuManagerReconcilePeriod')}")
print(f"  info  memoryManagerPolicy        {k.get('memoryManagerPolicy')}")
sys.exit(bad)
PY
}

if [[ "$VERIFY_ONLY" -eq 1 ]]; then
  verify && { echo; echo "CPU QoS configuration is correct on $NODE."; } || {
    echo; echo "CPU QoS configuration is NOT correct on $NODE - run: scripts/configure-cpu-qos.sh $NODE" >&2; exit 1; }
  exit 0
fi

# A node that is missing or NotReady cannot be drained, and neither state is this
# chapter's business to fix.
READY="$(kubectl get node "$NODE" -o jsonpath='{.status.conditions[?(@.type=="Ready")].status}' 2>/dev/null || true)"
if [[ -z "$READY" ]]; then
  echo "FATAL: node $NODE is not in the cluster. It has these nodes:" >&2
  kubectl get nodes 2>&1 | sed 's/^/         /' >&2
  echo "       scripts/install-k8s-cluster.sh joins every worker listed in" >&2
  echo "       config/nodes.env - finish docs/02-kubernetes-install.md first." >&2
  exit 2
elif [[ "$READY" != "True" ]]; then
  echo "FATAL: node $NODE is in the cluster but not Ready (Ready=$READY), so it" >&2
  echo "       cannot be drained. A NotReady node nearly always means its CNI is" >&2
  echo "       not running: see \"Calico never rolls out\" in" >&2
  echo "       docs/02-kubernetes-install.md." >&2
  exit 2
fi

echo "== 1/4 draining $NODE =="
# The static CPU Manager stores its allocations in cpu_manager_state. That file
# has to be removed for a policy change to take effect, and it can only be
# removed safely once no Pod holds an exclusive CPU.
kubectl cordon "$NODE"
kubectl drain "$NODE" --ignore-daemonsets --delete-emptydir-data --force --timeout=5m

echo "== 2/4 writing kubelet CPU QoS settings on $NODE =="
[[ -z "$LAB_REMOTE_SUDO" ]] || echo "Enter the worker sudo password when prompted."
"${LAB_REMOTE_SSH[@]}" "$TARGET" "$LAB_REMOTE_SUDO RESERVED='$RESERVED' python3 - <<'PY'
import os, shutil, yaml
path = '/var/lib/kubelet/config.yaml'
shutil.copy2(path, path + '.pre-cpu-qos')
with open(path) as fh:
    cfg = yaml.safe_load(fh)
cfg['cpuManagerPolicy'] = 'static'
cfg['cpuManagerPolicyOptions'] = {'full-pcpus-only': 'true', 'strict-cpu-reservation': 'true'}
cfg['cpuManagerReconcilePeriod'] = '10s'
cfg['reservedSystemCPUs'] = os.environ['RESERVED']
cfg['topologyManagerPolicy'] = 'single-numa-node'
cfg['topologyManagerScope'] = 'container'
cfg['cgroupDriver'] = 'systemd'
cfg['cgroupsPerQOS'] = True
with open(path, 'w') as fh:
    yaml.safe_dump(cfg, fh, sort_keys=True)
print('wrote ' + path + ' (previous copy: ' + path + '.pre-cpu-qos)')
PY
$LAB_REMOTE_SUDO rm -f /var/lib/kubelet/cpu_manager_state /var/lib/kubelet/memory_manager_state
$LAB_REMOTE_SUDO systemctl restart kubelet"

echo "== 3/4 waiting for $NODE to become Ready =="
kubectl wait --for=condition=Ready "node/$NODE" --timeout=5m
kubectl uncordon "$NODE"

echo "== 4/4 verifying =="
verify || { echo; echo "FATAL: settings did not take effect; check 'journalctl -u kubelet' on $NODE" >&2; exit 1; }
echo
echo "CPU QoS configured on $NODE. The 'pinned' scenario can now get exclusive cores."
