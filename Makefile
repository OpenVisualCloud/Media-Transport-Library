# Convenience wrappers around scripts/. Everything here is a one-line call to a
# script you can also run directly; nothing is hidden in the Makefile.
.PHONY: help setup preflight bios cluster cpu-qos cpu-qos-verify power power-verify \
        perfspect image worker \
        observability mcp density sweep rdt-host-a rdt-pod-a rdt-pod-b rdt-pod-c \
        summary test teardown sync clean

help:
	@echo "Setup, in order (see docs/QUICKSTART.md):"
	@echo "  make bios              1. check the worker BIOS against the BKC"
	@echo "  make cluster           2. install Kubernetes on every node"
	@echo "  make cpu-qos           3. apply the kubelet CPU QoS package"
	@echo "  make power             4. apply the power profile (pstate, governor, EPB/EPP/ELC)"
	@echo "  make perfspect         5. capture the PerfSpect platform baseline"
	@echo "  make image             6. build the FFmpeg-MXL workload image"
	@echo "  make worker            7a. worker side: PCM, RDT helper, stress-ng"
	@echo "  make observability     7b. Prometheus, Grafana, PCM scrape wiring"
	@echo "  make setup preflight   install the runner, then verify everything"
	@echo
	@echo "Measure:"
	@echo "  make density           baseline 12 / numa-pool 14 / pinned 20"
	@echo "  make sweep             walk stream counts to find your own limit"
	@echo "  make rdt-host-a        RDT policy sweep vs the host noisy neighbor"
	@echo "  make rdt-pod-a|b|c     RDT policy sweep vs each Pod noisy neighbor"
	@echo "  make summary           rebuild results/summary.{html,xlsx,csv}"
	@echo
	@echo "Maintenance:"
	@echo "  make cpu-qos-verify    re-check the kubelet CPU QoS package on the worker"
	@echo "  make power-verify      re-check the power profile on the worker"
	@echo "  make mcp               install the MCP profiling server on the worker"
	@echo "  make test              run the unit tests"
	@echo "  make teardown          delete the workload namespace"
	@echo "  make sync              rsync this repo to the controller"

setup:            ; scripts/setup.sh
preflight:        ; scripts/preflight.sh
bios:             ; scripts/check-bios.sh
cluster:          ; scripts/install-k8s-cluster.sh
cpu-qos:          ; scripts/configure-cpu-qos.sh
cpu-qos-verify:   ; scripts/configure-cpu-qos.sh --verify
power:            ; scripts/configure-power.sh
power-verify:     ; scripts/configure-power.sh --verify
perfspect:        ; scripts/run-perfspect.sh
image:            ; scripts/build-ffmpeg-mxl-image.sh
worker:           ; scripts/bootstrap-worker.sh
observability:    ; scripts/install-observability.sh
mcp:              ; scripts/install-mcp-profiler.sh

density:          ; scripts/run-campaign.sh campaigns/density.env
sweep:            ; scripts/run-campaign.sh campaigns/density-sweep.env
rdt-host-a:       ; scripts/run-campaign.sh campaigns/rdt-host-a.env
rdt-pod-a:        ; scripts/run-campaign.sh campaigns/rdt-pod-a.env
rdt-pod-b:        ; scripts/run-campaign.sh campaigns/rdt-pod-b.env
rdt-pod-c:        ; scripts/run-campaign.sh campaigns/rdt-pod-c.env
summary:          ; scripts/summarize.sh

# pytest comes from the dev extra that scripts/setup.sh installs into .venv/.
test:             ; ./.venv/bin/python -m pytest -q
teardown:         ; scripts/teardown.sh
sync:             ; scripts/sync-controller.sh

clean:
	rm -rf .venv .pytest_cache python/mxlperf/__pycache__ tests/__pycache__
