#!/usr/bin/env bash
# Check that everything a measured run needs is present, and name the installer
# for anything that is missing. Safe to run at any time; changes nothing.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/config/lab.env"
# shellcheck source=/dev/null
source "$ROOT/config/nodes.env"
: "${LAB_SSH_USER:?set LAB_SSH_USER in config/nodes.env}"
# shellcheck source=lib/no-proxy.sh
source "$ROOT/scripts/lib/no-proxy.sh"
# shellcheck source=lib/cpu-topology.sh
source "$ROOT/scripts/lib/cpu-topology.sh"
SHELL_NO_PROXY="$(lab_csv_merge "${no_proxy:-}" "${NO_PROXY:-}")"
lab_export_no_proxy   # or kubectl below reaches the API server through a proxy

fail=0
note() { printf '  %-6s %s\n' "$1" "$2"; [[ "$1" == "MISS" ]] && fail=1; return 0; }

echo "== controller tools =="
for cmd in kubectl python3 ssh scp; do
  if command -v "$cmd" >/dev/null; then note OK "$cmd"; else note MISS "$cmd is not installed"; fi
done
# helm installs the observability stack; once that is in the cluster, runs no
# longer need it, so a missing helm is a warning rather than a blocker.
if command -v helm >/dev/null; then note OK "helm"; else note WARN "helm absent - needed only by scripts/install-observability.sh"; fi
# Every script here excludes the cluster from the proxy for its own process. A
# kubectl you type yourself only gets that from your shell.
if [[ -z "${http_proxy:-}${HTTP_PROXY:-}${https_proxy:-}${HTTPS_PROXY:-}" ]]; then
  note OK "no proxy in this shell"
elif lab_csv_covers "$SHELL_NO_PROXY" "$(lab_cluster_no_proxy)"; then
  note OK "proxy set, and no_proxy covers the cluster"
else
  note WARN "proxy set, and no_proxy in this shell does not cover the cluster"
  echo "         The scripts here handle that themselves, but a kubectl you type will"
  echo "         answer Forbidden. See \"The API server answers 'Forbidden'\" in"
  echo "         docs/02-kubernetes-install.md."
fi

echo "== cluster =="
kubectl cluster-info >/dev/null && note OK "API server reachable"
if kubectl get crd podmonitors.monitoring.coreos.com >/dev/null 2>&1; then
  note OK "PodMonitor CRD present"
else
  note MISS "PodMonitor CRD absent - run scripts/install-observability.sh"
fi
if [[ -x "$ROOT/.venv/bin/mxl-perf" ]]; then
  note OK "mxl-perf installed"
else
  note MISS "mxl-perf missing - run scripts/setup.sh"
fi

IFS=, read -r -a workers <<<"$LAB_WORKERS"
for node in "${workers[@]}"; do
  key="$(printf '%s' "$node" | tr 'a-z-' 'A-Z_')_HOST"
  host="${!key:-}"
  echo "== worker $node (${host:-no SSH address in config/nodes.env}) =="
  kubectl get node "$node" -o jsonpath='  node   Ready={.status.conditions[?(@.type=="Ready")].status} cpu={.status.capacity.cpu} memory={.status.capacity.memory}{"\n"}'
  [[ -n "$host" ]] || { note MISS "no <NODE>_HOST entry in config/nodes.env"; continue; }
  if ! ssh -o BatchMode=yes -o ConnectTimeout=5 "$LAB_SSH_USER@$host" true 2>/dev/null; then
    note MISS "SSH as $LAB_SSH_USER@$host failed - run scripts/setup-controller-worker-ssh.sh"
    continue
  fi
  note OK "SSH reachable"
  controller_epoch="$(date +%s)"
  worker_epoch="$(ssh -o BatchMode=yes -o ConnectTimeout=5 "$LAB_SSH_USER@$host" date +%s 2>/dev/null || echo 0)"
  clock_skew=$((controller_epoch - worker_epoch))
  (( clock_skew < 0 )) && clock_skew=$((-clock_skew))
  if (( clock_skew <= 5 )); then
    note OK "clock skew ${clock_skew}s"
  else
    note MISS "clock skew ${clock_skew}s exceeds 5s - enable chrony on controller and worker"
  fi
  # The worker checks run in a remote shell, so their MISS lines have to be
  # carried back here to count towards the exit status.
  worker_report="$(ssh -o BatchMode=yes -o ConnectTimeout=5 "$LAB_SSH_USER@$host" "
    t() { printf '  %-6s %s\n' \"\$1\" \"\$2\"; }
    if [ \"\$(id -u)\" -eq 0 ]; then admin=''; else admin='sudo -n'; fi
    test -d '$LAB_MEDIA_HOSTPATH' && t OK 'media directory $LAB_MEDIA_HOSTPATH' || t MISS 'media directory $LAB_MEDIA_HOSTPATH absent - see docs/05-ffmpeg-mxl-container.md'
    test -f '$LAB_MEDIA_HOSTPATH/$LAB_INPUT_1080P' && t OK 'input clip $LAB_INPUT_1080P' || t MISS 'input clip $LAB_INPUT_1080P absent'
    if command -v nerdctl >/dev/null 2>&1 && nerdctl --namespace k8s.io image inspect '$LAB_IMAGE' >/dev/null 2>&1; then
      t OK 'workload image $LAB_IMAGE'
    elif command -v nerdctl >/dev/null 2>&1 && \$admin nerdctl --namespace k8s.io image inspect '$LAB_IMAGE' >/dev/null 2>&1; then
      t OK 'workload image $LAB_IMAGE'
    elif command -v crictl >/dev/null 2>&1 && \$admin crictl inspecti '$LAB_IMAGE' >/dev/null 2>&1; then
      t OK 'workload image $LAB_IMAGE'
    else
      t MISS 'workload image $LAB_IMAGE unavailable to the Kubernetes runtime - verify with: sudo nerdctl --namespace k8s.io image inspect $LAB_IMAGE'
    fi
    # The quotes inside awk's gsub have to be escaped: this whole remote script is
    # one double-quoted local string, so a bare \"\" closes and reopens it and awk
    # ends up with gsub(/ /,,\$2) - a syntax error, an empty value, and a topology
    # MISS on a machine that is configured correctly.
    threads_per_core=\$(lscpu | awk -F: '/^Thread\\(s\\) per core/{gsub(/ /,\"\",\$2); print \$2}')
    cores_per_socket=\$(lscpu | awk -F: '/^Core\\(s\\) per socket/{gsub(/ /,\"\",\$2); print \$2}')
    if [ \"\$threads_per_core\" = '${LAB_THREADS_PER_CORE:-1}' ]; then t OK \"topology has \$threads_per_core threads/core\"; else t MISS \"topology has \$threads_per_core threads/core but LAB_THREADS_PER_CORE=${LAB_THREADS_PER_CORE:-1}; every Guaranteed CPU request is sized from it\"; fi
    if [ \"\$cores_per_socket\" = '$LAB_CORES_PER_SOCKET' ]; then t OK \"topology has \$cores_per_socket cores/socket\"; else t MISS \"topology has \$cores_per_socket cores/socket but LAB_CORES_PER_SOCKET=$LAB_CORES_PER_SOCKET\"; fi
    grep -q 'cpuManagerPolicy: static' /var/lib/kubelet/config.yaml 2>/dev/null && t OK 'kubelet static CPU Manager' || t WARN 'cannot confirm static CPU Manager - run scripts/configure-cpu-qos.sh (pinned scenario needs it)'
    # Power: the governor is a MISS because a ramping governor costs whole streams
    # and would be read as a property of the platform. The driver mode and EPB are
    # WARN - they matter, but not every kernel exposes them.
    gov=\$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | tr '\n' ',')
    if [ \"\$gov\" = '${LAB_POWER_GOVERNOR:-performance},' ]; then t OK \"governor ${LAB_POWER_GOVERNOR:-performance} on every CPU\"; else t MISS \"governor \${gov%,} but LAB_POWER_GOVERNOR=${LAB_POWER_GOVERNOR:-performance} - run scripts/configure-power.sh\"; fi
    pstate=\$(cat /sys/devices/system/cpu/intel_pstate/status 2>/dev/null || echo absent)
    if [ '${LAB_POWER_PSTATE_DRIVER:-active}' = skip ] || [ \"\$pstate\" = '${LAB_POWER_PSTATE_DRIVER:-active}' ]; then t OK \"intel_pstate status \$pstate\"; else t WARN \"intel_pstate status \$pstate but LAB_POWER_PSTATE_DRIVER=${LAB_POWER_PSTATE_DRIVER:-active} - run scripts/configure-power.sh\"; fi
    epb=\$(cat /sys/devices/system/cpu/cpu*/power/energy_perf_bias 2>/dev/null | sort -u | tr '\n' ',')
    if [ -z \"\$epb\" ]; then t WARN 'energy_perf_bias not exposed by this kernel'
    elif [ \"\$epb\" = '${LAB_POWER_EPB:-0},' ]; then t OK \"energy_perf_bias \${epb%,} on every CPU\"
    else t WARN \"energy_perf_bias \${epb%,} but LAB_POWER_EPB=${LAB_POWER_EPB:-0} - run scripts/configure-power.sh\"; fi
    systemctl is-enabled --quiet mxl-power-profile.service 2>/dev/null && t OK 'power profile re-applied at boot' || t WARN 'mxl-power-profile.service not enabled - EPB/EPP/ELC are lost on the next reboot; run scripts/configure-power.sh'
    systemctl is-active --quiet pcm-sensor-server.service && t OK 'Intel PCM exporter active' || t MISS 'Intel PCM exporter inactive - run scripts/bootstrap-worker.sh, then scripts/install-observability.sh'
    inst=\$(sysctl -n fs.inotify.max_user_instances 2>/dev/null || echo 0)
    # ~2 inotify instances per stream, shared with kubelet/containerd/systemd.
    if [ \"\$inst\" -ge 8192 ]; then t OK \"inotify instances \$inst\"; else t MISS \"fs.inotify.max_user_instances=\$inst is too low for dense runs - run scripts/install-worker-limits.sh\"; fi
    test -x /usr/local/sbin/mxl-rdt-host && t OK 'RDT helper installed' || t MISS 'RDT helper absent - run scripts/install-rdt-host.sh'
    findmnt -n -t resctrl >/dev/null 2>&1 && t OK 'resctrl mounted' || t MISS 'resctrl not mounted - run scripts/install-rdt-host.sh'
    command -v stress-ng >/dev/null && t OK 'stress-ng present (host noisy neighbor)' || t MISS 'stress-ng absent - run scripts/install-host-noise.sh'
    \$admin /usr/sbin/dmidecode -t memory >/dev/null 2>&1 && t OK 'DMI memory probe allowed' || t WARN 'DMI memory probe denied - theoretical DRAM peak stays unavailable; run scripts/install-platform-probe.sh'
    lscpu | grep -qE 'cat_l3' && t OK 'CPU reports L3 CAT' || t WARN 'CPU does not report cat_l3; RDT control unavailable'
    lscpu | grep -qE '\bmba\b' && t OK 'CPU reports MBA' || t WARN 'CPU does not report mba; MBA profiles unavailable'
  " || true)"
  printf '%s\n' "$worker_report"
  if grep -q '^  MISS' <<<"$worker_report"; then fail=1; fi

  # Runs on the controller, where the awk over lscpu needs no remote quoting.
  if [[ "${LAB_THREADS_PER_CORE:-1}" != "1" ]]; then
    core_map="$(ssh -o BatchMode=yes -o ConnectTimeout=5 "$LAB_SSH_USER@$host" "lscpu -p=CPU,CORE" 2>/dev/null || true)"
    partial="$(lab_partially_reserved_cores "${LAB_RESERVED_CPUS:-}" "$core_map" | tr '\n' ' ')"
    if [[ -z "$core_map" ]]; then
      note WARN "could not read the CPU-to-core map; whole-core reservation unchecked"
    elif [[ -n "${partial// /}" ]]; then
      note MISS "LAB_RESERVED_CPUS reserves only part of core(s) ${partial% } - under full-pcpus-only those cores become unusable; reserve both siblings"
    else
      note OK "LAB_RESERVED_CPUS covers whole cores"
    fi
  fi
done

echo
if [[ "$fail" -eq 0 ]]; then
  echo "Preflight passed."
else
  echo "Preflight found missing components (MISS lines above). Fix them before measuring." >&2
  exit 1
fi
