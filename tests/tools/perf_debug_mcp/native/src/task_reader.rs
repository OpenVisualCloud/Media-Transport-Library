//! Fast batch task info reader.

use serde::Serialize;

#[derive(Serialize)]
pub struct TaskInfo {
    pub pid: i32,
    pub cpus: Vec<u32>,
    pub policy: String,
    pub priority: i32,
    pub nice: i32,
    pub error: Option<String>,
}

/// Read affinity + scheduler info for multiple PIDs in a single call.
pub fn batch_task_info(pids: &[i32]) -> anyhow::Result<Vec<TaskInfo>> {
    let mut results = Vec::with_capacity(pids.len());

    for &pid in pids {
        let aff = crate::affinity::get_affinity(pid);
        let sched = crate::scheduler::get_scheduler(pid);

        match (aff, sched) {
            (Ok(a), Ok(s)) => {
                results.push(TaskInfo {
                    pid,
                    cpus: a.cpus,
                    policy: s.policy,
                    priority: s.priority,
                    nice: s.nice,
                    error: None,
                });
            }
            (Ok(a), Err(e)) => {
                results.push(TaskInfo {
                    pid,
                    cpus: a.cpus,
                    policy: "UNKNOWN".to_string(),
                    priority: 0,
                    nice: 0,
                    error: Some(format!("scheduler: {}", e)),
                });
            }
            (Err(e), _) => {
                results.push(TaskInfo {
                    pid,
                    cpus: vec![],
                    policy: "UNKNOWN".to_string(),
                    priority: 0,
                    nice: 0,
                    error: Some(e.to_string()),
                });
            }
        }
    }

    Ok(results)
}
