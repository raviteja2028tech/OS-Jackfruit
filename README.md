🚀 Multi-Container Runtime with Kernel Memory Monitor
👤 Student Information

Name: Raviteja
Course: Computer Science Engineering
(Add USN if required)

📌 Project Overview

This project implements a lightweight container runtime in C that demonstrates core operating system concepts such as process isolation, inter-process communication, memory monitoring, and CPU scheduling.

The system consists of:

A user-space supervisor to manage container lifecycle
A kernel module for monitoring and enforcing memory limits
A logging system using pipes and a bounded buffer
A CLI interface for interacting with containers
Scheduling experiments to observe CPU allocation behavior
⚙️ Setup and Execution
1. Install Dependencies
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
2. Build the Project
make
3. Load Kernel Module
sudo insmod monitor.ko
ls -l /dev/container_monitor
4. Prepare Root Filesystem
mkdir rootfs-base

wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz

tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
5. Start Supervisor
sudo ./engine supervisor ./rootfs-base
6. Start Containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine start beta ./rootfs-beta /bin/sh
7. List Running Containers
sudo ./engine ps
8. View Logs
sudo ./engine logs <container_id>
9. Stop Container
sudo ./engine stop <container_id>
10. Unload Kernel Module
sudo rmmod monitor
🧪 Features Demonstrated
Multi-Container Execution

Multiple containers can run concurrently under a single supervisor process.

Container Monitoring

The ps command displays:

Container ID
Process ID (PID)
State (running/exited)
Memory limits
Logging System

Container output is captured using:

Pipes
Producer-consumer model
Bounded buffer
CLI and IPC

Commands are communicated via UNIX domain sockets between CLI and supervisor.

Memory Management
Soft Limit: Generates warning
Hard Limit: Terminates container
Scheduling Behavior

CPU usage varies based on process priority (nice values).

Clean Process Handling
No zombie processes
Proper cleanup after execution
🧠 Engineering Concepts
Container Isolation

Isolation is achieved using Linux namespaces:

PID Namespace → Separate process IDs
UTS Namespace → Independent hostname
Mount Namespace + chroot → Filesystem isolation

Each container runs in an isolated environment.

Supervisor Design

The supervisor process:

Manages multiple containers
Maintains container metadata
Handles signals (SIGCHLD)
Cleans up processes using waitpid()
Inter-Process Communication

Two communication paths are used:

1. Logging Path

Container → Pipe → Supervisor
Uses producer-consumer model

2. Control Path

CLI → UNIX socket → Supervisor

Synchronization is handled using:

Mutex
Condition variables
Memory Monitoring

Memory usage is measured using RSS (Resident Set Size).

Soft limit → Warning
Hard limit → Process termination

Kernel-space implementation ensures accurate monitoring.

CPU Scheduling Experiment
yes > /dev/null &
yes > /dev/null &

sudo renice -5 <PID1>
sudo renice 10 <PID2>

Observation:

Lower nice value → higher CPU priority
Higher nice value → lower CPU usage

Linux uses the Completely Fair Scheduler (CFS) to balance CPU allocation.

⚖️ Design Decisions
Component	Approach	Trade-off
Filesystem Isolation	chroot	Simpler but less secure
IPC	UNIX sockets	Easy to implement
Logging	Bounded buffer	Prevents overflow
Memory Control	Kernel module	Accurate but complex
📊 Scheduling Results
Process	Nice Value	CPU Usage
Process 1	-5	High (~100%)
Process 2	10	Lower (~80%)
🧹 Resource Management
All child processes are properly terminated
No zombie processes
Threads exit cleanly
Kernel memory released after module unload
🏁 Conclusion

This project demonstrates:

Containerization using Linux namespaces
Kernel-level resource monitoring
Concurrent logging systems
CPU scheduling behavior

It provides a simplified implementation of a container runtime, showcasing key principles behind modern systems like Docker.
