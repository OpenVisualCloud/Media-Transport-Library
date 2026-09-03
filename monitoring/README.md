# Monitoring dashboards

This branch vendors the Grafana dashboards needed for CRM observability under
[`monitoring/dashboards/`](dashboards) so they can be imported directly into a
Grafana instance backed by the repository's Prometheus datasource (uid
`prometheus`). When adding a new Grafana dashboard you can either upload these
JSON files directly or open one of the files in
[`monitoring/dashboards/`](dashboards) and copy/paste its contents into
Grafana's import dialog.

## Grafana dashboards

| File | Title | Purpose |
|------|-------|---------|
| `dashboards/system-overview-grafana-dashboard.json` | CRM — System Overview | **Default operational view.** Fast-loading (15 min window) health dashboard covering workload FPS, node CPU/memory, DRAM bandwidth, package/DRAM power, thermal headroom, IPC, UPI, PCIe, and observability status. Start here. |
| `dashboards/fps-grafana-dashboard.json` | MXL FFmpeg FPS | Application-level encoder/decoder FPS, speed ratio, and frame throughput. |
| `dashboards/pcm-grafana-dashboard.json` | Intel PCM — k8s-w2 (FFmpeg-MXL Scenario 1) | Concise CRM workload view for DRAM bandwidth, LLC behaviour, PCIe traffic, package power, and active frequency during scenario runs. |
| `dashboards/pcm-extended-grafana-dashboard.json` | Intel PCM — Extended Hardware Counters (Deep Dive) | **Deep-dive investigation view.** Detailed hardware counters: core/thread activity, IPC, L2/L3 cache, per-socket DRAM, power, frequency, PCIe, thermal headroom, and measurement interval. |
| `dashboards/perfspect-grafana-dashboard.json` | Intel PerfSpect — k8s-w2 (FFmpeg-MXL Scenario 1) | Top-down microarchitecture, IPC/CPI, NUMA, and latency analysis from the PerfSpect collector. |

> **PMU is single-owner:** Intel PCM and Intel PerfSpect cannot own the PMU simultaneously.
> Keep both dashboards installed if you want, but only the active collector will emit data.
>
> **LLC occupancy note:** this repository's live Grafana dashboards use Prometheus-fed
> PCM/PerfSpect/FFmpeg metrics. Per-cgroup LLC occupancy is collected by the Intel
> RDT workflow and written into run results, so it is documented in the
> observability docs rather than queried live from the PCM dashboard JSON.

## Importing a dashboard

1. Open Grafana and navigate to **Dashboards → Import**.
2. Either click **Upload JSON file** and select a file from
   [`monitoring/dashboards/`](dashboards), or open one of the JSON files there
   (for example
   [`dashboards/pcm-extended-grafana-dashboard.json`](dashboards/pcm-extended-grafana-dashboard.json))
   and copy/paste its contents into the import dialog.
3. When Grafana prompts for a datasource, choose **Prometheus** with uid `prometheus`.
4. Click **Import**.

The FPS dashboard includes a `namespace` variable that defaults to
`crm-workloads`; if your encoder/decoder Pods run elsewhere, update that
variable after import.

The System Overview dashboard also includes a `namespace` variable (same
default) and an `instance` variable that is auto-populated from the PCM
exporter targets.

