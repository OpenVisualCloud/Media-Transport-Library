# Lcore Guide

## 1. Introduction

In DPDK (Data Plane Development Kit), an "lcore" stands for "logical core," and it represents a logical CPU core on a multi-core processor. Binding a thread to a specific logical core (lcore) is a technique used to achieve better control over the execution of packet processing tasks and to optimize the performance of networking applications.

It minimizes the impact of OS scheduler decisions, reduces cache-related issues, and allows for fine-grained control over CPU resources, all of which are critical for meeting the stringent performance requirements of networking workloads. Intel® Media Transport Library scheduler used pinned lcore also as performance consideration.

## 2. Lcore manager for multi process

IMTL supports multi-process deployment based on SR-IOV VF isolation. To manage the dispatching of lcores among multiple processes, it introduces shared memory mapping to maintain the status of lcore usage. Each process searches the mapping and allocates a new lcore slot from it, freeing the slot when it is no longer in use.
The last user attached to the shared memory will reset the mapping to an initial status during the `shmdt` operation as part of the release routine.

IMTL also provides a tool which can be find from `./build/app/LcoreMgr` for manually cleaning up lcore entries. This tool is typically used when a process fails to release lcores due to panic issues or other failures that prevent the regular free routine from running. Below is the usage information:

```bash
  --info: Print lcore shared manager detail info
  --clean_pid_auto_check: Clean the dead entries if PID is not active
  --clean_lcore <lcore id>: Clean the entry by lcore ID
```

When you use the `--clean_pid_auto_check` option, the tool will perform a loop check for all the active entries in the map. It checks whether the hostname and user match the current login environment and then verifies if the PID is still running. If the tool detects that the PID is no longer active, it will proceed to remove the lcore from the mapping.

If you are deploying in a multi-container environment, the PID check becomes less useful as each container has its own process namespace. In such cases, you can use the --clean_lcore option to remove an entry based on the lcore ID. However, it's important to confirm that the lcore is not active before using this option.