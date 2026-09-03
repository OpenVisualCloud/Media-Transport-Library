#!/usr/bin/env bash
# Observability stack.
#
# Installs kube-prometheus-stack (Prometheus, Grafana, node-exporter,
# kube-state-metrics) and wires in the host Intel PCM exporter on every worker.
#
# Three metric sources, each answering something the others cannot:
#   node-exporter  per-CPU utilisation, so a report can show which cores worked
#   cAdvisor       per-container CPU time, for encoder vs decoder attribution
#   host PCM       UPI cross-socket traffic, DRAM read/write, L3 hit ratio -
#                  whole-socket counters no in-cluster exporter can see
#   (FPS itself comes from a sidecar in each encoder Pod, scraped through a
#    PodMonitor the runner creates per run.)
#
# Run FROM THE CONTROLLER:
#   scripts/install-observability.sh
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/config/lab.env"
# shellcheck source=lib/no-proxy.sh
source "$ROOT/scripts/lib/no-proxy.sh"
# helm still needs the proxy to fetch the chart; only the cluster is excluded.
lab_export_no_proxy

command -v helm >/dev/null || { echo "FATAL: helm is not installed (https://helm.sh/docs/intro/install/)" >&2; exit 2; }
RELEASE="${LAB_PROM_RELEASE:-monitoring}"

echo "== 1/3 kube-prometheus-stack =="
helm repo add prometheus-community https://prometheus-community.github.io/helm-charts >/dev/null
helm repo update >/dev/null
helm upgrade --install "$RELEASE" prometheus-community/kube-prometheus-stack \
  --namespace monitoring --create-namespace \
  -f "$ROOT/observability/kube-prometheus-values.yaml" --wait
kubectl -n monitoring rollout status "daemonset/$RELEASE-prometheus-node-exporter" --timeout=5m

echo "== 2/3 host PCM scrape targets =="
IFS=, read -r -a workers <<<"$LAB_WORKERS"
for node in "${workers[@]}"; do
  node_ip="$(kubectl get node "$node" -o jsonpath='{.status.addresses[?(@.type=="InternalIP")].address}')"
  [[ -n "$node_ip" ]] || { echo "FATAL: no InternalIP for node $node" >&2; exit 1; }
  if ! curl -fsS --max-time 5 \
      "http://$node_ip:9738/metrics" 2>/dev/null |
      grep -F 'Incoming_Data_Traffic_On_Link_' >/dev/null; then
    echo "FATAL: $node ($node_ip:9738) is not serving UPI counters." >&2
    echo "Run scripts/bootstrap-worker.sh $node, then re-run this script." >&2
    exit 1
  fi
  sed -e "s/__NODE__/$node/g" -e "s/__NODE_IP__/$node_ip/g" \
    "$ROOT/observability/pcm-host-scrape.yaml" | kubectl apply -f -
  echo "  wired $node ($node_ip:9738)"
done

echo "== 3/3 confirming Prometheus sees PCM =="
# A fresh Operator must reconcile the ServiceMonitor, reload Prometheus,
# discover the EndpointSlice and complete its first scrape. That can take much
# longer than one scrape interval even though every component is healthy.
QUERY_PATH="/api/v1/namespaces/monitoring/services/$RELEASE-kube-prometheus-prometheus:9090/proxy/api/v1/query?query=up%7Bjob%3D%22pcm-sensor-server%22%7D"
prometheus_response=""
prometheus_error=""
for _ in {1..60}; do
  if prometheus_response="$(kubectl -n monitoring get --raw "$QUERY_PATH" 2>/tmp/mxl-prometheus-query.err)"; then
    if grep -q '"value"[^]]*"1"' <<<"$prometheus_response"; then
      echo "  PCM target is up."
      prometheus_error=""
      break
    fi
  else
    prometheus_error="$(cat /tmp/mxl-prometheus-query.err)"
  fi
  sleep 2
done
rm -f /tmp/mxl-prometheus-query.err
if [[ -n "$prometheus_error" || ! "$prometheus_response" =~ \"value\"[^]]*\"1\" ]]; then
  echo "FATAL: Prometheus did not report a healthy PCM target within 120s." >&2
  if [[ -n "$prometheus_error" ]]; then
    echo "       Prometheus API error: $prometheus_error" >&2
  elif grep -q '"value"' <<<"$prometheus_response"; then
    echo "       Target was discovered but its latest scrape returned up=0." >&2
  else
    echo "       ServiceMonitor has not produced a PCM target." >&2
  fi
  echo "       Inspect: kubectl -n monitoring get servicemonitor,endpointslice pcm-sensor-server -o yaml" >&2
  exit 1
fi

cat <<EOF

Observability ready.
  Prometheus: scripts/port-forward-prometheus.sh   then http://127.0.0.1:19090
  Grafana:    kubectl -n monitoring port-forward svc/$RELEASE-grafana 13000:80
              user admin, password: kubectl -n monitoring get secret $RELEASE-grafana \\
                -o jsonpath='{.data.admin-password}' | base64 -d
EOF
