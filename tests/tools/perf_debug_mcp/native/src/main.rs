//! cpu-debug-ebpf — Native helper binary for cpu-debug-mcp
//!
//! Provides fast procfs reading, syscall-based affinity/scheduler queries,
//! and optional eBPF CO-RE scheduling/IRQ/softirq data collection.
//!
//! Commands:
//!   capabilities          — Report available features
//!   sched_snapshot <json> — Collect scheduling data via eBPF
//!   task_affinity <json>  — Read task affinity via syscall

mod affinity;
mod scheduler;
mod task_reader;

#[cfg(feature = "ebpf")]
mod ebpf;

use serde::{Deserialize, Serialize};
use std::env;

#[derive(Serialize)]
struct CapabilitiesResponse {
    ebpf_available: bool,
    missing_caps: Vec<String>,
    helper_version: String,
}

#[derive(Serialize)]
struct ErrorResponse {
    error: String,
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        print_usage();
        std::process::exit(1);
    }

    let command = &args[1];
    let params = if args.len() > 2 { Some(&args[2]) } else { None };

    let result = match command.as_str() {
        "capabilities" => handle_capabilities(),
        "sched_snapshot" => handle_sched_snapshot(params),
        "task_affinity" => handle_task_affinity(params),
        "task_scheduler" => handle_task_scheduler(params),
        "batch_task_info" => handle_batch_task_info(params),
        _ => {
            eprintln!("Unknown command: {}", command);
            print_usage();
            std::process::exit(1);
        }
    };

    match result {
        Ok(json) => {
            println!("{}", json);
        }
        Err(e) => {
            let err = ErrorResponse { error: e.to_string() };
            println!("{}", serde_json::to_string(&err).unwrap());
            std::process::exit(1);
        }
    }
}

fn print_usage() {
    eprintln!("Usage: cpu-debug-ebpf <command> [params_json]");
    eprintln!("Commands:");
    eprintln!("  capabilities          — Report available features");
    eprintln!("  sched_snapshot <json> — Collect scheduling data via eBPF");
    eprintln!("  task_affinity <json>  — Read task affinity via syscall");
    eprintln!("  task_scheduler <json> — Read task scheduler via syscall");
    eprintln!("  batch_task_info <json>— Read multiple tasks' info");
}

fn handle_capabilities() -> anyhow::Result<String> {
    let mut missing_caps = Vec::new();
    let mut ebpf_available = false;

    // Check for eBPF support
    #[cfg(feature = "ebpf")]
    {
        ebpf_available = ebpf::check_ebpf_available(&mut missing_caps);
    }

    #[cfg(not(feature = "ebpf"))]
    {
        missing_caps.push("compiled without ebpf feature".to_string());
    }

    // Check Linux capabilities
    check_capabilities(&mut missing_caps);

    let resp = CapabilitiesResponse {
        ebpf_available,
        missing_caps,
        helper_version: env!("CARGO_PKG_VERSION").to_string(),
    };

    Ok(serde_json::to_string(&resp)?)
}

fn check_capabilities(missing: &mut Vec<String>) {
    // Read /proc/self/status for CapBnd (bounding set)
    if let Ok(content) = std::fs::read_to_string("/proc/self/status") {
        for line in content.lines() {
            if line.starts_with("CapBnd:") || line.starts_with("CapEff:") {
                if let Some(hex_str) = line.split_whitespace().nth(1) {
                    if let Ok(caps) = u64::from_str_radix(hex_str.trim(), 16) {
                        // CAP_BPF = 39
                        if caps & (1u64 << 39) == 0 {
                            missing.push("CAP_BPF".to_string());
                        }
                        // CAP_PERFMON = 38
                        if caps & (1u64 << 38) == 0 {
                            missing.push("CAP_PERFMON".to_string());
                        }
                        // CAP_SYS_ADMIN = 21
                        if caps & (1u64 << 21) == 0 {
                            missing.push("CAP_SYS_ADMIN".to_string());
                        }
                        break; // only check first matching line
                    }
                }
            }
        }
    }
}

fn handle_sched_snapshot(params: Option<&String>) -> anyhow::Result<String> {
    #[cfg(feature = "ebpf")]
    {
        #[derive(Deserialize)]
        struct Params {
            window_ms: u64,
        }
        let p: Params = match params {
            Some(s) => serde_json::from_str(s)?,
            None => return Err(anyhow::anyhow!("Missing params")),
        };
        let snapshot = ebpf::collect_sched_snapshot(p.window_ms)?;
        Ok(serde_json::to_string(&snapshot)?)
    }
    #[cfg(not(feature = "ebpf"))]
    {
        Err(anyhow::anyhow!("eBPF support not compiled in"))
    }
}

fn handle_task_affinity(params: Option<&String>) -> anyhow::Result<String> {
    #[derive(Deserialize)]
    struct Params {
        pid: i32,
    }
    let p: Params = match params {
        Some(s) => serde_json::from_str(s)?,
        None => return Err(anyhow::anyhow!("Missing params")),
    };
    let aff = affinity::get_affinity(p.pid)?;
    Ok(serde_json::to_string(&aff)?)
}

fn handle_task_scheduler(params: Option<&String>) -> anyhow::Result<String> {
    #[derive(Deserialize)]
    struct Params {
        pid: i32,
    }
    let p: Params = match params {
        Some(s) => serde_json::from_str(s)?,
        None => return Err(anyhow::anyhow!("Missing params")),
    };
    let sched = scheduler::get_scheduler(p.pid)?;
    Ok(serde_json::to_string(&sched)?)
}

fn handle_batch_task_info(params: Option<&String>) -> anyhow::Result<String> {
    #[derive(Deserialize)]
    struct Params {
        pids: Vec<i32>,
    }
    let p: Params = match params {
        Some(s) => serde_json::from_str(s)?,
        None => return Err(anyhow::anyhow!("Missing params")),
    };
    let results = task_reader::batch_task_info(&p.pids)?;
    Ok(serde_json::to_string(&results)?)
}
