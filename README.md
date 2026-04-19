# Multi-Container Runtime with Kernel Memory Monitor

## 👥 Team Information

* A RAVITEJA(PES1UG24AM001)
* AKARSH KUMAR GUPTA(PES1UG24AM021)

---

# 📌 Project Summary

This project implements a lightweight Linux container runtime in C with:

* A **user-space supervisor** to manage multiple containers
* A **kernel module** to monitor and enforce memory limits
* A **logging system** using a bounded buffer and pipes
* A **CLI interface** for container lifecycle management
* **Scheduling experiments** demonstrating Linux CPU scheduling behavior

---

# ⚙️ Build and Run Instructions

## 🔹 Step 1: Install Dependencies

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

---

## 🔹 Step 2: Build Project

```bash
make
```

---

## 🔹 Step 3: Load Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

---

## 🔹 Step 4: Setup Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

---

## 🔹 Step 5: Start Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

---

## 🔹 Step 6: Run Containers

```bash
sudo ./engine start c1 ./rootfs-alpha /bin/sh
sudo ./engine start c2 ./rootfs-beta /bin/sh
```

---

## 🔹 Step 7: View Containers

```bash
sudo ./engine ps
```

---

## 🔹 Step 8: Logs

```bash
sudo ./engine logs <container_id>
```

---

## 🔹 Step 9: Stop Containers

```bash
sudo ./engine stop <container_id>
```

---

## 🔹 Step 10: Unload Module

```bash
sudo rmmod monitor
```

---

# 📸 Demo and Screenshots

## 1. Multi-container supervision

Multiple containers running concurrently under one supervisor.

## 2. Metadata tracking

`engine ps` displays:

* Container ID
* PID
* State
* Memory limits

## 3. Logging system

Logs captured using pipe → bounded buffer → file.

## 4. CLI + IPC

Commands sent via UNIX socket to supervisor and executed.

## 5. Soft limit warning

Kernel logs show warning when memory crosses soft limit.

## 6. Hard limit enforcement

Container killed when memory exceeds hard limit.

## 7. Scheduling experiment

Different CPU usage observed based on nice values.

## 8. Clean teardown

No zombie processes after container termination.

---

# 🧠 Engineering Analysis

## 1. Isolation Mechanisms

Isolation is achieved using:

* **PID namespace** → separate process trees
* **UTS namespace** → hostname isolation
* **Mount namespace + chroot** → filesystem isolation

Each container runs in its own root filesystem, preventing interference.

---

## 2. Supervisor and Process Lifecycle

A long-running supervisor:

* Manages multiple containers
* Tracks metadata
* Handles signals (SIGCHLD)
* Prevents zombie processes using `waitpid()`

---

## 3. IPC, Threads, and Synchronization

Two IPC mechanisms:

### Path A (Logging)

* Pipes from container → supervisor
* Producer-consumer model
* Bounded buffer prevents overflow

### Path B (Control)

* UNIX domain socket for CLI communication

Synchronization:

* Mutex + condition variables used
* Prevents race conditions and data loss

---

## 4. Memory Management and Enforcement

* RSS (Resident Set Size) used to measure memory
* Soft limit → warning
* Hard limit → SIGKILL

Kernel-space enforcement ensures:

* accuracy
* security
* real-time monitoring

---

## 5. Scheduling Behavior

### Experiment: CPU vs CPU

```bash
yes > /dev/null &
yes > /dev/null &
sudo renice -5 <PID1>
sudo renice 10 <PID2>
```

### Observation:

* nice = -5 → ~100% CPU
* nice = 10 → ~83% CPU

### Explanation:

Linux Completely Fair Scheduler (CFS):

* prioritizes lower nice values
* distributes CPU fairly across processes
* allows parallel execution on multi-core systems

---

# ⚖️ Design Decisions and Tradeoffs

| Component      | Choice         | Trade off                               |
| -------------- | -------------- | --------------------------------------- |
| Filesystem     | chroot         | simpler but less secure than pivot_root |
| IPC            | UNIX socket    | simpler than shared memory              |
| Logging        | bounded buffer | prevents overflow but adds complexity   |
| Memory control | kernel module  | harder to implement but more accurate   |

---

# 📊 Scheduler Experiment Results

| Process   | Nice | CPU Usage |
| --------- | ---- | --------- |
| Process 1 | -5   | ~100%     |
| Process 2 | 10   | ~83%      |

Conclusion:

* Lower nice → higher priority
* Scheduler ensures fairness while respecting priority

---

# 🧹 Resource Cleanup

* All child processes reaped
* No zombie processes
* Threads terminated cleanly
* Kernel memory freed on module unload

---

# 🏁 Conclusion

This project demonstrates:

* Containerization using Linux namespaces
* Kernel-level resource monitoring
* Concurrent logging system
* Process scheduling behavior

It provides a simplified but functional container runtime similar to Docker, showcasing key operating system concepts in practice.

---

