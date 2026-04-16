//! CPU affinity reader via sched_getaffinity syscall.

use nix::sched::{sched_getaffinity, CpuSet};
use nix::unistd::Pid;
use serde::Serialize;

#[derive(Serialize)]
pub struct AffinityInfo {
    pub pid: i32,
    pub cpus: Vec<u32>,
    pub hex_mask: String,
}

/// Get CPU affinity for a task (by TID/PID).
pub fn get_affinity(pid: i32) -> anyhow::Result<AffinityInfo> {
    let cpu_set = sched_getaffinity(Pid::from_raw(pid))?;
    let mut cpus = Vec::new();
    
    // Check up to 1024 CPUs
    for i in 0..1024u32 {
        if cpu_set.is_set(i as usize).unwrap_or(false) {
            cpus.push(i);
        }
    }

    // Build hex mask
    let mut mask: u128 = 0;
    for &cpu in &cpus {
        if cpu < 128 {
            mask |= 1u128 << cpu;
        }
    }
    let hex_mask = format!("{:x}", mask);

    Ok(AffinityInfo {
        pid,
        cpus,
        hex_mask,
    })
}
