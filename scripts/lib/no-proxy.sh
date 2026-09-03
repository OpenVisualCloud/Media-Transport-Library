# shellcheck shell=bash
# Sourced by every script here that talks to the API server. Not run directly.
#
# One job: keep the cluster's own traffic out of an HTTP proxy.
#
# kubectl, helm and the Python runner all honour http_proxy/https_proxy from the
# environment, and the kubeconfig points at the control plane's own address on
# port 6443. A proxy asked to CONNECT there normally refuses with 403, which
# kubectl prints as
#
#   Unable to connect to the server: Forbidden
#
# naming everything except the cause. The cluster installer fixes this on the
# nodes, in /etc/environment - but the machine you drive the repo from may be a
# third one that no installer has touched, so every script fixes it for its own
# process here.
#
# Nothing is overridden: entries are merged into whatever no_proxy already says,
# and in a shell with no proxy set the whole thing is a no-op.

lab_csv_merge() {   # lab_csv_merge <csv>... -> the union, in order, no duplicates
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

lab_csv_covers() {   # lab_csv_covers <have> <want> -> 0 when every want token is in have
  local have="$1" want="$2" tok toks
  IFS=, read -r -a toks <<<"$want"
  for tok in "${toks[@]}"; do
    [[ -n "$tok" ]] || continue
    case ",$have," in *",$tok,"*) ;; *) return 1 ;; esac
  done
}

# Everything that must never go through a proxy: the cluster's CIDRs, and every
# address and node name in config/nodes.env. Read from that file directly rather
# than from the environment, so this works in a script that sources neither
# config file.
lab_cluster_no_proxy() {   # lab_cluster_no_proxy [extra-entry ...] -> csv
  local root line name addr node ip want
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  want="localhost,127.0.0.1,${SERVICE_CIDR:-10.96.0.0/12},${POD_CIDR:-10.244.0.0/16}"
  want+=",.svc,.cluster.local"
  while read -r line; do
    name="${line%%=*}"
    addr="${line#*=}"; addr="${addr%%#*}"; addr="${addr//\"/}"; addr="${addr//\'/}"
    [[ -n "$addr" ]] || continue
    # The key is the node name uppercased with - as _, so this is that in reverse.
    node="${name%_HOST}"; node="${node//_/-}"
    want+=",$addr,${node,,}"
    # nodes.env may hold a DNS name while the kubeconfig holds the address it
    # resolved to, so exclude both spellings of the same host.
    ip="$( { getent ahostsv4 "$addr" || true; } | awk 'NR==1{print $1}')"
    [[ -z "$ip" || "$ip" == "$addr" ]] || want+=",$ip"
  done < <( { grep -E '^[[:space:]]*[A-Za-z0-9_]+_HOST=' "$root/config/nodes.env" 2>/dev/null || true; } \
            | tr -d '[:blank:]')
  lab_csv_merge "$want" "$@"
}

# Export it for this script and everything it starts. Silent on a direct
# connection, and silent when no_proxy already covers the cluster.
lab_export_no_proxy() {   # lab_export_no_proxy [extra-entry ...]
  local before merged
  [[ -n "${http_proxy:-}${HTTP_PROXY:-}${https_proxy:-}${HTTPS_PROXY:-}" ]] || return 0
  before="$(lab_csv_merge "${no_proxy:-}" "${NO_PROXY:-}")"
  merged="$(lab_csv_merge "$before" "$(lab_cluster_no_proxy "$@")")"
  if [[ "$merged" != "$before" ]]; then
    echo "-- a proxy is set in this shell; excluding the cluster's own addresses from it" >&2
  fi
  no_proxy="$merged"
  NO_PROXY="$merged"
  export no_proxy NO_PROXY
}
