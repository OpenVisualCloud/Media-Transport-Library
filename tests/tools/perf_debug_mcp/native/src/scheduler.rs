//! Scheduler policy reader via sched_getscheduler / sched_getparam.

use serde::Serialize;

#[derive(Serialize)]
pub struct SchedulerInfo {
    pub pid: i32,
    pub policy: String,
    pub policy_num: i32,
    pub priority: i32,
    pub nice: i32,
}

/// Get scheduling policy and priority for a task.
pub fn get_scheduler(pid: i32) -> anyhow::Result<SchedulerInfo> {
    // Use raw syscalls for sched_getscheduler and sched_getparam
    let policy_num = unsafe { libc::sched_getscheduler(pid) };
    if policy_num < 0 {
        return Err(anyhow::anyhow!(
            "sched_getscheduler failed: {}",
            std::io::Error::last_os_error()
        ));
    }

    let mut param: libc::sched_param = unsafe { std::mem::zeroed() };
    let ret = unsafe { libc::sched_getparam(pid, &mut param) };
    let priority = if ret == 0 { param.sched_priority } else { 0 };

    // Get nice value (only meaningful for SCHED_OTHER/BATCH)
    let nice = unsafe { libc::getpriority(libc::PRIO_PROCESS, pid as u32) };

    let policy = match policy_num {
        0 => "SCHED_OTHER",
        1 => "SCHED_FIFO",
        2 => "SCHED_RR",
        3 => "SCHED_BATCH",
        5 => "SCHED_IDLE",
        6 => "SCHED_DEADLINE",
        _ => "UNKNOWN",
    };

    Ok(SchedulerInfo {
        pid,
        policy: policy.to_string(),
        policy_num,
        priority,
        nice,
    })
}
