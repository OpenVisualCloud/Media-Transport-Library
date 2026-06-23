//! eBPF collector module — loads CO-RE BPF programs for sched_switch,
//! sched_wakeup, irq_handler, and softirq tracepoints.
//!
//! This module is only compiled when the "ebpf" feature is enabled.

#[cfg(feature = "ebpf")]
use libbpf_rs::{MapCore, MapFlags, ObjectBuilder};
use serde::Serialize;
#[allow(unused_imports)]
use std::collections::HashMap;
use std::path::Path;
use std::time::Duration;

#[derive(Serialize, Clone)]
pub struct SchedEntry {
    pub cpu: u32,
    pub pid: u32,
    pub tid: u32,
    pub comm: String,
    pub runtime_ns: u64,
    pub switches: u32,
}

#[derive(Serialize, Clone)]
pub struct IrqEntry {
    pub cpu: u32,
    pub irq: u32,
    pub time_ns: u64,
    pub count: u32,
}

#[derive(Serialize, Clone)]
pub struct SoftirqEntry {
    pub cpu: u32,
    pub vec: u32,
    pub time_ns: u64,
    pub count: u32,
}

#[derive(Serialize)]
pub struct SchedSnapshot {
    pub sched: Vec<SchedEntry>,
    pub irq: Option<Vec<IrqEntry>>,
    pub softirq: Option<Vec<SoftirqEntry>>,
    pub window_ms: u64,
}

/// Check if eBPF is available on this system.
pub fn check_ebpf_available(missing_caps: &mut Vec<String>) -> bool {
    // Check if /sys/kernel/btf/vmlinux exists (needed for CO-RE)
    if !Path::new("/sys/kernel/btf/vmlinux").exists() {
        missing_caps.push("BTF not available (/sys/kernel/btf/vmlinux missing)".to_string());
        return false;
    }

    // Check if we can use bpf() syscall
    // Simple probe: try to create a minimal prog
    // For now, check capabilities
    let status = std::fs::read_to_string("/proc/self/status").unwrap_or_default();
    for line in status.lines() {
        if line.starts_with("CapEff:") {
            if let Some(hex) = line.split_whitespace().nth(1) {
                if let Ok(caps) = u64::from_str_radix(hex.trim(), 16) {
                    if caps & (1u64 << 39) == 0 {
                        missing_caps.push("CAP_BPF not in effective caps".to_string());
                    }
                    if caps & (1u64 << 38) == 0 {
                        missing_caps.push("CAP_PERFMON not in effective caps".to_string());
                    }
                    // If either is missing, might still work with CAP_SYS_ADMIN
                    if caps & (1u64 << 21) != 0 {
                        return true; // CAP_SYS_ADMIN implies BPF access
                    }
                    return (caps & (1u64 << 39) != 0) && (caps & (1u64 << 38) != 0);
                }
            }
        }
    }

    missing_caps.push("Could not read capabilities".to_string());
    false
}

/// Collect a scheduling snapshot over the given window.
#[cfg(feature = "ebpf")]
pub fn collect_sched_snapshot(window_ms: u64) -> anyhow::Result<SchedSnapshot> {
    // The BPF object file should be embedded or found alongside the binary
    let bpf_obj_path = find_bpf_object("sched_switch.bpf.o")?;
    
    let mut builder = ObjectBuilder::default();
    let open_obj = builder.open_file(&bpf_obj_path)?;
    let mut obj = open_obj.load()?;
    
    // Attach to sched_switch tracepoint
    let prog = obj.progs_mut()
        .find(|p| p.name().to_string_lossy() == "handle_sched_switch")
        .ok_or_else(|| anyhow::anyhow!("BPF program 'handle_sched_switch' not found"))?;
    let _link = prog.attach()?;
    
    // Sleep for the window
    std::thread::sleep(Duration::from_millis(window_ms));
    
    // Read the map
    let map = obj.maps()
        .find(|m| m.name().to_string_lossy() == "task_runtime")
        .ok_or_else(|| anyhow::anyhow!("BPF map 'task_runtime' not found"))?;
    
    let mut sched_entries = Vec::new();
    
    // Iterate map entries using keys() iterator
    for key in map.keys() {
        if let Ok(Some(value)) = map.lookup(&key, MapFlags::ANY) {
            // Parse key: {cpu: u32, pid: u32, tid: u32}
            if key.len() >= 12 && value.len() >= 20 {
                let cpu = u32::from_ne_bytes(key[0..4].try_into().unwrap());
                let pid = u32::from_ne_bytes(key[4..8].try_into().unwrap());
                let tid = u32::from_ne_bytes(key[8..12].try_into().unwrap());
                let runtime_ns = u64::from_ne_bytes(value[0..8].try_into().unwrap());
                let switches = u32::from_ne_bytes(value[8..12].try_into().unwrap());
                let comm_bytes = &value[12..28.min(value.len())];
                let comm = String::from_utf8_lossy(
                    &comm_bytes[..comm_bytes.iter().position(|&b| b == 0).unwrap_or(comm_bytes.len())]
                ).to_string();
                
                sched_entries.push(SchedEntry {
                    cpu, pid, tid, comm, runtime_ns, switches,
                });
            }
        }
    }
    
    // Sort by runtime desc
    sched_entries.sort_by(|a, b| b.runtime_ns.cmp(&a.runtime_ns));
    
    Ok(SchedSnapshot {
        sched: sched_entries,
        irq: None,
        softirq: None,
        window_ms,
    })
}

#[cfg(feature = "ebpf")]
fn find_bpf_object(name: &str) -> anyhow::Result<std::path::PathBuf> {
    // Search relative to the binary
    let exe_dir = std::env::current_exe()?
        .parent()
        .unwrap_or(Path::new("."))
        .to_path_buf();
    
    let search_paths = [
        exe_dir.join(name),
        exe_dir.join("bpf").join(name),
        Path::new("/usr/lib/cpu-debug-mcp/bpf").join(name),
        Path::new("./bpf").join(name),
    ];
    
    for path in &search_paths {
        if path.exists() {
            return Ok(path.clone());
        }
    }
    
    Err(anyhow::anyhow!("BPF object '{}' not found. Searched: {:?}", name, search_paths))
}

#[cfg(not(feature = "ebpf"))]
pub fn collect_sched_snapshot(_window_ms: u64) -> anyhow::Result<SchedSnapshot> {
    Err(anyhow::anyhow!("eBPF support not compiled in"))
}
