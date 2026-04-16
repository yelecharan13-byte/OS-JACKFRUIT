# OS Jackfruit Project

## Team Members

YELE CHARAN - PES1UG25CS856  
VIJAYKUMAR - PES1UG24CS529  



## Build, Load, and Run Instructions
Prerequisites
Ubuntu 22.04 or 24.04 in a VM (Secure Boot OFF, no WSL)
sudo apt install -y build-essential linux-headers-$(uname -r)
Download Alpine rootfs
mkdir rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs
Build everything
make
This builds: engine, memory_hog, cpu_hog, io_pulse, and monitor.ko.

Load kernel module
sudo insmod monitor.ko
# Allow dmesg access
sudo sysctl kernel.dmesg_restrict=0
# Verify device created
ls -la /dev/container_monitor
Start supervisor (Terminal 1)
sudo ./engine supervisor ./rootfs
Container operations (Terminal 2)
# Start a container in background
sudo ./engine start alpha ./rootfs /bin/sh

# Start a container and wait for it
sudo ./engine run test1 ./rootfs /bin/hostname

# List all containers
sudo ./engine ps

# View container logs
sudo ./engine logs alpha

# Stop a container
sudo ./engine stop alpha
Run memory limit test
sudo cp memory_hog ./rootfs/
sudo ./engine start memtest ./rootfs /memory_hog --soft-mib 5 --hard-mib 10
dmesg | grep container_monitor
Run scheduler experiments
sudo cp cpu_hog ./rootfs/
sudo cp io_pulse ./rootfs/

# Experiment 1: Different priorities
sudo ./engine start cpu_hi ./rootfs /cpu_hog --nice 0
sudo ./engine start cpu_lo ./rootfs /cpu_hog --nice 19
sleep 12
cat logs/cpu_hi.log
cat logs/cpu_lo.log

# Experiment 2: CPU-bound vs I/O-bound
sudo ./engine start cpu_w ./rootfs /cpu_hog --nice 0
sudo ./engine start io_w  ./rootfs /io_pulse --nice 0
sleep 12
cat logs/cpu_w.log
cat logs/io_w.log
Clean shutdown
# Ctrl+C on supervisor terminal, then:
sudo rmmod monitor
dmesg | grep container_monitor | tail -5

## Screenshots

### 1. CPU Hog Running
![1](01.png)

### 2. Process Check
![2](02.png)

### 3. Kernel Logs
![3](03.png)

### 4. Memory Hog Running
![4](04.png)

### 5. Memory Process
![5](05.png)

### 6. System Logs
![6](06.png)

### 7. Combined View
![7](07.png)

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms
The runtime achieves process and filesystem isolation through Linux namespaces and chroot.

Namespaces are created using clone() with three flags:

CLONE_NEWPID — gives the container a private PID namespace. The first process inside sees itself as PID 1. The host kernel still tracks the real PID, but the container cannot see or signal host processes.  

CLONE_NEWUTS — gives the container its own hostname. Changing the hostname inside the container does not affect the host.  

CLONE_NEWNS — gives the container a private mount namespace. Mounts inside (like /proc) are invisible to the host.  

chroot changes the container's root filesystem to the Alpine rootfs directory. The container can only access files inside that tree. The host filesystem is not visible.

All containers share the same kernel. The kernel enforces isolation at the syscall boundary.

---

### 4.2 Supervisor and Process Lifecycle
A long-running supervisor is required because containers are child processes.

The supervisor uses clone() to create containers with namespaces.  
It keeps track of all container PIDs and handles SIGCHLD to clean up exited processes using waitpid().

Without this, zombie processes would accumulate.

---

### 4.3 IPC, Threads, and Synchronization
The project uses:

- Pipes for capturing container logs  
- UNIX domain socket for CLI commands  

A bounded buffer is used for logging with:
- mutex for synchronization  
- condition variables for producer-consumer coordination  

This prevents race conditions and ensures safe concurrent access.

---

### 4.4 Memory Management and Enforcement
Memory usage is tracked using RSS (Resident Set Size), which represents actual RAM usage.

Two limits are used:
- Soft limit → warning  
- Hard limit → process is killed  

Enforcement is done in kernel space to avoid delays and race conditions present in user-space monitoring.

---

### 4.5 Scheduling Behavior
Linux uses Completely Fair Scheduler (CFS).

- nice = 0 → weight 1024  
- nice = 19 → weight 15  

Lower nice value gets more CPU share.

### Design Decisions and Tradeoffs
Namespace Isolation
Choice: PID + UTS + mount namespaces via clone(), chroot for filesystem isolation. Tradeoff: No network namespace — containers share the host network stack, which means port conflicts are possible. Justification: Network namespace setup requires significant additional plumbing (veth pairs, bridge setup). For the scope of this project, demonstrating PID/UTS/mount isolation is sufficient and keeps the implementation auditable.

Supervisor Architecture
Choice: Single long-running supervisor process with a UNIX socket event loop. Tradeoff: The event loop is single-threaded, so a slow CLI command (e.g. streaming a large log) blocks other commands briefly. Justification: A single-threaded event loop with select() is far simpler to reason about for signal safety. Multi-threaded accept loops require careful signal masking. For a course project, correctness over throughput is the right call.

IPC and Logging
Choice: Pipe for log data, UNIX domain socket for control. Tradeoff: Pipes are unidirectional and per-container, so N containers need N pipes. A shared memory ring buffer would be more efficient. Justification: Pipes are the natural fit for capturing stdout/stderr — dup2 into the pipe fd is two lines of code. They are also self-closing: when the container exits, the pipe EOF wakes the producer thread to exit cleanly.

Kernel Monitor
Choice: Mutex to protect the monitored list; mutex_trylock in the timer callback. Tradeoff: mutex_trylock means we skip a monitoring tick if the lock is held. A missed tick is acceptable — the process will be checked on the next tick 1 second later. Justification: A spinlock in timer context would disable preemption while spinning, which can cause latency spikes on the entire system. The mutex with trylock is safer and the 1-second timer period makes a skipped tick inconsequential.

Scheduler Experiments
Choice: Measure accumulated work (accumulator value) as the metric, not wall-clock time. Tradeoff: Accumulator values depend on CPU speed and vary across machines; they are not portable metrics. Justification: Both containers run the same binary for the same duration, so the accumulator is a fair relative measure of CPU time received. Absolute values don't matter — the ratio does.

## 6. Scheduler Experiment Results

### Experiment 1: CPU-bound vs CPU-bound, different nice values
Both containers ran /cpu_hog for 10 seconds simultaneously.

Container    Nice value    CFS weight    Final accumulator  
cpu_hi2      0             1024          10,732,451,889,245,672,310  
cpu_lo2      19            15            3,421,998,556,783,120,845  

Ratio: cpu_hi2 / cpu_lo2 ≈ 3.13x  

Analysis:  
Linux CFS gave cpu_hi2 more CPU time proportional to its higher weight. The theoretical weight ratio is 1024/15 ≈ 68x, but the observed ratio is around ~3x because other processes running on the system also share CPU resources.  
This confirms that higher priority results in more CPU share.

---

### Experiment 2: CPU-bound vs I/O-bound, same nice value
Both containers ran at nice=0 simultaneously.

Container    Type        Duration    CPU behavior  
cpu_w        CPU-bound   10s         Continuous computation, high CPU usage  
io_w         I/O-bound   ~4–5s       Completed 20 iterations with periodic sleep  

Analysis:  
The CPU-bound process continuously utilized CPU resources.  
The I/O-bound process frequently performed sleep operations, which caused it to release the CPU.  

When the I/O-bound process woke up, it was scheduled quickly due to lower virtual runtime.  
This demonstrates that I/O-bound processes remain responsive because they do not continuously compete for CPU time.


