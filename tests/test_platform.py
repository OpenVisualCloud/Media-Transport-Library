from mxlperf.platform import parse_platform, platform_summary_line

PROBE_OUTPUT = """=== lscpu-json ===
{
   "lscpu": [
      {"field": "CPU(s):", "data": "128"},
      {"field": "Model name:", "data": "Intel(R) Xeon(R) 6767P"},
      {"field": "Thread(s) per core:", "data": "1"},
      {"field": "Core(s) per socket:", "data": "64"},
      {"field": "Socket(s):", "data": "2"},
      {"field": "CPU max MHz:", "data": "3900.0000"},
      {"field": "Caches (sum of all):", "data": null, "children": [
         {"field": "L3 cache:", "data": "672 MiB (2 instances)"}
      ]},
      {"field": "NUMA:", "data": null, "children": [
         {"field": "NUMA node(s):", "data": "2"}
      ]}
   ]
}
=== meminfo ===
MemTotal:       527976388 kB
=== dimms ===
dimm CPU_SrcID#0_MC#0_Chan#0_DIMM#0 32768 Unbuffered-DDR5
dimm CPU_SrcID#0_MC#1_Chan#0_DIMM#0 32768 Unbuffered-DDR5
dimm CPU_SrcID#1_MC#0_Chan#0_DIMM#0 32768 Unbuffered-DDR5
dimm CPU_SrcID#1_MC#1_Chan#0_DIMM#0 32768 Unbuffered-DDR5
dimm CPU_SrcID#1_MC#2_Chan#0_DIMM#0 0 Unknown
=== dmi-memory ===
=== power ===
scaling_driver intel_pstate
pstate_status active
no_turbo 0
governor performance
epb 0
epp performance
"""


def test_platform_probe_is_parsed() -> None:
    spec = parse_platform(PROBE_OUTPUT)
    assert spec["cpu_model"] == "Intel(R) Xeon(R) 6767P"
    assert spec["sockets"] == 2
    assert spec["cores_per_socket"] == 64
    assert spec["logical_cpus"] == 128
    assert spec["numa_nodes"] == 2
    assert spec["l3_cache"] == "672 MiB (2 instances)"
    assert spec["populated_dimms"] == 4
    assert spec["populated_channels"] == 4
    assert spec["channels_per_socket"] == 2
    assert spec["memory_total_gib"] == 503.5


def test_peak_is_unavailable_without_transfer_rate() -> None:
    spec = parse_platform(PROBE_OUTPUT)
    assert spec["memory_transfer_mt_s"] is None
    assert spec["memory_transfer_source"] == ""
    assert "theoretical_dram_gbps" not in spec
    assert "theoretical DRAM peak unavailable" in platform_summary_line(spec)


def test_declared_transfer_rate_produces_peak() -> None:
    spec = parse_platform(PROBE_OUTPUT, 6400)
    assert spec["memory_transfer_source"] == "declared"
    # 4 channels x 6400 MT/s x 8 bytes.
    assert spec["theoretical_dram_gbps"] == 204.8


def test_probed_dmi_rate_wins_over_declaration() -> None:
    # Inserted into the section rather than appended to the output: the power
    # section comes after dmi-memory, as it does in the real probe.
    raw = PROBE_OUTPUT.replace(
        "=== dmi-memory ===\n",
        "=== dmi-memory ===\n\tConfigured Memory Speed: 5600 MT/s\n\tSpeed: 6400 MT/s\n",
    )
    spec = parse_platform(raw, 4800)
    assert spec["memory_transfer_source"] == "dmi"
    assert spec["memory_transfer_mt_s"] == 6400


def test_power_settings_are_recorded() -> None:
    spec = parse_platform(PROBE_OUTPUT)
    assert spec["power"] == {
        "scaling_driver": "intel_pstate",
        "pstate_status": "active",
        "no_turbo": "0",
        "governor": "performance",
        "epb": "0",
        # Under the performance governor intel_pstate pins EPP and sysfs reports
        # the name, not the number LAB_POWER_EPP asked for.
        "epp": "performance",
    }


def test_mixed_per_cpu_power_values_are_kept_distinct() -> None:
    raw = PROBE_OUTPUT.replace("governor performance", "governor performance,powersave")
    spec = parse_platform(raw)
    # Not averaged and not reduced to the first value: performance on all but one
    # CPU is not a performance run, and the comma is what makes that visible.
    assert spec["power"]["governor"] == "performance,powersave"


def test_missing_probe_output_is_safe() -> None:
    spec = parse_platform("")
    assert spec["populated_dimms"] == 0
    assert spec["sockets"] is None
    # Every power field present but empty: an older kernel exposing none of them
    # is not the same as a wrong value.
    assert spec["power"] == {
        "scaling_driver": "",
        "pstate_status": "",
        "no_turbo": "",
        "governor": "",
        "epb": "",
        "epp": "",
    }
    assert platform_summary_line({}) == "unavailable"
