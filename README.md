🚀 Multi-Container Runtime with Kernel Memory Monitor
👥 Student Information

Raviteja (PES1UG2AM001)
AKARSH (PES1UG24AM021)


📌 Project Summary

This project implements a lightweight Linux container runtime in C that demonstrates core operating system concepts.

The system includes:

A user-space supervisor to manage multiple containers
A kernel module for monitoring and enforcing memory limits
A logging system using pipes and a bounded buffer
A command-line interface (CLI) for container lifecycle management
Scheduling experiments to observe CPU behavior
⚙️ Build and Run Instructions
🔹 Step 1: Install Dependencies
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
🔹 Step 2: Build Project
make
🔹 Step 3: Load Kernel Module
sudo insmod monitor.ko
ls -l /dev/container_monitor
🔹 Step 4: Setup Root Filesystem
mkdir rootfs-base

wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz

tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
🔹 Step 5: Start Supervisor
sudo ./engine supervisor ./rootfs-base
🔹 Step 6: Run Containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine start beta ./rootfs-beta /bin/sh
🔹 Step 7: View Containers
sudo ./engine ps
🔹 Step 8: View Logs
sudo ./engine logs <container_id>
🔹 Step 9: Stop Containers
sudo ./engine stop <container_id>
🔹 Step 10: Unload Kernel Module
sudo rmmod monitor
📸 Features Demonstrated
1. Multi-container Execution

Multiple containers can run simultaneously under a single supervisor process.

2. Container Metadata Tracking

The engine ps command displays:

Container ID
Process ID (PID)
Current state (running/exited/killed)
Memory limits
3. Logging System

Container outputs are captured using:

Pipes
Bounded buffer
Log storage
4. CLI + IPC

Commands are sent to the supervisor using UNIX domain sockets.

5. Memory Monitoring
Soft limit → generates warning
Hard limit → terminates container
6. Scheduling Experiment

Demonstrates CPU allocation differences using nice values.

7. Clean Process Handling
No zombie processes
Proper process termination
Safe cleanup of resources
🧠 Engineering Concepts
1. Container Isolation

Isolation is achieved using Linux namespaces:

PID namespace → separate process IDs
UTS namespace → independent hostname
Mount namespace + chroot → filesystem isolation

Each container operates in its own environment.

2. Supervisor Design

The supervisor:

Manages all containers
Maintains metadata
Handles signals (SIGCHLD)
Prevents zombie processes using waitpid()
3. IPC and Synchronization
Logging Path
Pipe from container → supervisor
Producer-consumer model
Bounded buffer prevents overflow
Control Path
UNIX domain socket used for communication
Synchronization
Mutex and condition variables
Ensures thread-safe operations
4. Memory Monitoring
Memory usage measured using RSS
Kernel module enforces limits

Behavior:

Soft limit → warning
Hard limit → SIGKILL

Kernel-level monitoring ensures accuracy and reliability.

5. CPU Scheduling Behavior

Experiment:

yes > /dev/null &
yes > /dev/null &
sudo renice -5 <PID1>
sudo renice 10 <PID2>
Observation:
Lower nice value → higher CPU usage
Higher nice value → lower priority
Explanation:

Linux uses the Completely Fair Scheduler (CFS) to balance CPU usage while respecting priorities.

⚖️ Design Decisions
Component	Choice	Trade-off
Filesystem	chroot	simpler but less secure
IPC	UNIX socket	simpler implementation
Logging	bounded buffer	prevents overflow but adds complexity
Memory control	kernel module	accurate but complex
📊 Scheduling Results
Process	Nice Value	CPU Usage
Process 1	-5	High (~100%)
Process 2	10	Lower (~80%)
🧹 Resource Cleanup
All child processes are properly terminated
No zombie processes
Threads exit cleanly
Kernel memory freed on module removal
🏁 Conclusion

This project demonstrates:

Container creation using Linux namespaces
Kernel-level memory monitoring
Logging with concurrent processing
CPU scheduling behavior

It provides a simplified container runtime that captures key concepts behind modern systems like Docker.
