# amogus-numa-daemon

`amogus-numa-daemon` is a fork of `numad`: a Linux NUMA daemon that monitors
system topology and workload placement, then improves CPU and memory locality
to reduce unnecessary remote access costs.

This fork extends the original idea with a stronger focus on modern
multi-socket and heterogeneous systems, including:

- safer and more robust `/proc` parsing
- corrected internal NUMA node handling (node index vs node ID, CPU ID vs
  online CPU count)
- GPU-aware locality support for AMD systems
- compatibility with `sched_ext`-based schedulers
- rebind-stability controls (cooldowns, anti-ping-pong, hysteresis)
- improved packaging and service integration

## Goals

The main goal of this project is to keep workloads as local as possible:

- keep CPU-heavy tasks close to their memory
- keep GPU-related tasks close to the CPU and NUMA domain nearest to the GPU
- avoid unnecessary page migration and affinity churn
- cooperate with external schedulers

This project is designed for mixed environments, including:

- CPU-only workloads
- GPGPU workloads
- games running through Wine
- multi-socket NUMA servers

## Current behavior

`amogus-numa-daemon` remains a PID/TID-centric daemon.

It does **not** move to a cgroup-first or service-first model. Instead, it
continues to:

- inspect processes through `/proc`
- compute their effective load and locality needs
- choose preferred NUMA nodes
- set CPU affinity
- optionally migrate memory when that is actually beneficial

Explicitly included PIDs (`-p`) are treated as always manageable and are
exempt from the admission thresholds.

The daemon runs as a background process (systemd `Type=forking`), registers
`/var/run/numad.pid`, and logs to `/var/log/numad.log`. `SIGHUP` reopens the
log file, `SIGTERM`/`SIGQUIT` shut the daemon down cleanly.

## AMD GPU awareness

GPU-aware placement is implemented for AMD GPUs (sysfs vendor `0x1002`).
Set `--gpu-aware=0` to disable all of it.

### Latest Implemented features
03.08.2026 (by amogus-hermes-bot)
- GPU topology discovery through sysfs / DRM (`/sys/class/drm/cardN`),
  refreshed every 60 seconds
- GPU-local NUMA node resolution from `device/numa_node` and
  `device/local_cpulist`
- GPU-aware NUMA node preference with a generic-placement fallback
- CPU affinity narrowing to GPU-local CPUs (with a big-memory exception, see
  below)
- switchable graphics placement policy: `auto`, `prefer`, or `strict`
- DRM `fdinfo`-based process activity tracking (full `/proc` scan every
  `--gpu-fdinfo-discovery` seconds; respects `-S 0`)
- threshold-based automatic GPU admission: a workload must meet
  `--gpu-min-busy` **or** `--gpu-min-vram` from a fresh `fdinfo` sample;
  merely having an open GPU fd is no longer enough
- stale GPU state aging and cleanup (a sample older than two discovery
  intervals is treated as stale)
- safer handling of low-RSS but GPU-significant processes

### Backend notes

The implemented process telemetry path is based on **DRM `fdinfo`**
(`/proc/<pid>/fdinfo`).

The `amdsmi` backend option is currently not implemented: selecting
`--gpu-backend=amdsmi` logs a warning and falls back to `fdinfo`.

The `fdinfo` parser understands both the legacy `drm-memory-vram:` key and the
modern per-region keys `drm-resident-vram:` / `drm-total-vram:` (the legacy
key wins; otherwise the resident set is preferred over the total). Engine
lines (`drm-engine-*`) are classified by engine family: `compute`/`gfx` count
as compute, `sdma`/`dma`/copy engines count as neither, and everything else
(`vcn`, `uvd`, `vce`, `jpeg`, ...) counts as graphics. Compute and graphics
flags are tracked independently; a process with both is reported as `mixed`.

### Graphics placement policy

`--gpu-graphics-placement=auto|prefer|strict` controls how graphics-oriented
GPU workloads are placed:

- `auto` (default): try GPU-local NUMA nodes first; if they cannot satisfy the
  request, widen to generic NUMA placement while still preserving GPU-local
  NUMA membership in the final target set.
- `prefer`: the explicit soft mode. Placement behavior is identical to `auto`
  (GPU-local nodes are tried first, and the generic fallback still preserves
  GPU-local membership); the difference is only in the logged policy name.
- `strict`: the hard mode. When a graphics process has valid GPU-local NUMA
  node information and those nodes cannot satisfy the request, the fallback
  is refused and the current binding is kept.

The policy only affects node selection. Memory migration stays controlled
separately by `--gpu-migrate`. With `--gpu-migrate=auto`, pure graphics
workloads avoid aggressive page migration, and processes busier than
`--gpu-migrate-busy-max` (default 20 %) are left alone so a busy inference
job is not disrupted mid-run; memory migration is retried on the next
cooldown window.

### CPU affinity narrowing and the big-memory exception

For GPU-active processes the CPU affinity mask is normally narrowed to the
GPU-local CPUs, keeping compute threads close to the device.

This narrowing is applied **only while the process's memory target is
contained within the GPU-local nodes**. A big-memory process whose working set
spans several nodes (for example an AI inference process holding experts in
RAM across multiple NUMA nodes) keeps the full node-derived CPU mask, so its
threads are allowed to spread across every target node instead of stacking on
the GPU node's CPUs. GPU processes with a small, GPU-local footprint keep the
narrowed mask as before.

## Placement stability (anti-bounce behavior)

The daemon intentionally avoids constant rebinding. Once a process has been
placed, the following guards apply:

- **Bind cooldown** (`--bind-cooldown`, default 300 s): a process is not
  re-evaluated for `bind-cooldown` seconds after a successful bind, a
  suppressed move, or a locality refresh.
- **Anti-ping-pong**: returning to the node set the process was moved *from*
  is suppressed while the cooldown is active.
- **Hysteresis**: a move must improve memory locality by at least 10 % to be
  applied.
- **Headroom check**: if the current nodes still satisfy the requested CPU
  and memory, the process stays put.
- **Already-localized check**: if at least `-m <N>` (default 90 %) of the
  process's memory is on the target nodes and the target equals the current
  binding, no rebind happens (only the cooldown timer is refreshed).
- **Same-target skip**: a repeated bind toward the last successful target is
  skipped while the cooldown is active.
- **Drift repair**: if the process's affinity drifted from the last
  successful target (for example another tool changed it), the daemon
  re-applies the last target even during the cooldown.

Failed `migrate_pages` attempts (EINVAL/EFAULT/EPERM/...) abort only the
memory migration, not the affinity update: the affinity already applied is
credited with a fresh `bind_time_stamp` (so the process gets the cooldown and
is not re-evaluated every cycle), the reason is written to the log, and the
migration is retried after the cooldown. A process that actually died during
migration is still detected by the `/proc` liveness check.

## `sched_ext` compatibility

The daemon detects whether `sched_ext` is enabled and which scheduler is
active (`beerland`, `p2dq`, or something else) by reading
`/sys/kernel/sched_ext/state` and `/sys/kernel/sched_ext/root/ops`.

`--scx-mode` selects the interaction level:

- `cooperate` (default) and `legacy`: normal operation. The daemon never
  expands a process's allowed mask, only constrains it, so affinity
  management does not fight with `sched_ext` schedulers that rely on their
  own task placement.
- `observe`: dry-run mode. The daemon logs what it *would* rebind but applies
  no affinity or memory changes.

`--scx-sched=auto|beerland|p2dq` records the scheduler the system is expected
to run; it does not change placement behavior.

### Recommended schedulers

For NUMA systems, the preferred schedulers are:

- `scx_beerland`
- `scx_p2dq`

### Generally not recommended for this use case

For gaming, AI inference, and other tasks, latency is completely irrelevant.
Especially for large NUMA systems, ensuring data locality is far more
important; therefore, these schedulers are generally NOT RECOMMENDED:

- `scx_lavd`
- `scx_bpfland`
- `scx_rustland`
- `scx_flash`

`scx_rusty` is also not the preferred first choice for multi-socket NUMA
systems.

## Options

```
-C 0|1          count inactive file cache as available memory (default 1)
-d              debug logging (same as -l 7)
-h              usage
-H <N>          THP scan_sleep_ms (default 1000; 0 keeps the system default)
-i [<MIN>:]<MAX>  interval seconds (default 5:15); with a running daemon,
                max 0 (or "0:0") shuts the daemon down
-K 0|1          merge / keep interleaved memory (default 0 = merge)
-l <N>          log level, 0..7, syslog levels (default 5 = LOG_NOTICE)
-m <N>          memory locality target percent (default 90)
-p <PID>        add PID to the inclusion list (always manageable)
-r <PID>        remove PID from the explicit PID lists
-R <CPU_LIST>   reserve CPUs for non-numad use (excluded from node capacity)
-S 0|1          scan all processes (default 1); 0 = only explicit PID list
-t <N>          logical CPU / thread valuation percent (default 20)
-u <N>          utilization target percent (default 90)
-v              verbose (same as -l 6)
-V              version
-w <CPUs>[:<MBs>]  pre-placement NUMA advice, e.g. "4.25:2000" (no daemon
                needed; answered via the message queue when one is running)
-x <PID>        add PID to the exclusion list
```

Startup-only long options (see `man numad` for details):

```
--gpu-aware=0|1                    (default 1)
--gpu-backend=auto|fdinfo|amdsmi   (default auto; amdsmi falls back to fdinfo)
--gpu-min-busy=<N>                 (default 10)
--gpu-min-vram=<MB>                (default 256)
--gpu-fdinfo-discovery=<sec>       (default 15)
--gpu-migrate=auto|always|never    (default auto)
--gpu-migrate-busy-max=<N>         (default 20)
--gpu-graphics-placement=auto|prefer|strict  (default auto)
--scx-mode=legacy|cooperate|observe (default cooperate)
--scx-sched=auto|beerland|p2dq     (default auto)
--bind-cooldown=<sec>              (default 300)
```

The GPU/SCX long options and `--bind-cooldown` are startup-only: if a daemon
is already running they are rejected with a hint to edit `/etc/numad.conf`
and restart the service. All short options are dynamic and are delivered to a
running daemon through its message queue.

## Message queue and client model

Subsequent invocations communicate with a running daemon through a SysV
message queue (`msgget`, key `0xaddaadda`):

- the queue is created with `0600 | IPC_CREAT | IPC_EXCL`;
- if the queue already exists, the daemon verifies ownership (`uid` and
  `cuid` must match its euid) and tightens any group/other permissions left
  over from older versions;
- clients therefore must run as the same user as the daemon (root under the
  packaged systemd service). A non-root user cannot inject commands or read
  replies from a queue they do not own.

This prevents an unprivileged user from pre-creating the well-known key and
injecting commands (`-p`/`-x`/`-i`, including the shutdown request) into the
root daemon.

## Configuration

The daemon reads configuration from:

```
/etc/numad.conf
```

The shipped configuration sets `INTERVAL=15` and a `NUMAD_ARGS` string that
enables GPU-aware operation:

```
NUMAD_ARGS="-i 5:15 --gpu-aware=1 --gpu-backend=auto --gpu-min-busy=10
  --gpu-min-vram=256 --gpu-fdinfo-discovery=15 --gpu-migrate=auto
  --gpu-migrate-busy-max=20 --gpu-graphics-placement=prefer
  --scx-mode=cooperate --bind-cooldown=300 -S 1"
```

`NUMAD_ARGS` takes precedence over `INTERVAL`. The wrapper sources the file
with `sh`, so values are shell-expanded; keep them whitespace-separated and
quote anything containing shell metacharacters.

The service wrapper is:

```
/usr/lib/numad/numad-wrapper
```

A systemd unit file is included (`numad.service`, `Type=forking`) plus a
SysV-style init script (`numad.init`) for non-systemd environments. Log
rotation is handled by `/etc/logrotate.d/numad` (1 MB per file, 5 files).

### Typical usage

```
sudo systemctl daemon-reload
sudo systemctl enable --now numad.service
```

## Packaging

An Arch Linux PKGBUILD is included (`amogus-numa-daemon-git`). The package
keeps the historical installed file names:

```
/usr/bin/numad
/usr/share/man/man8/numad.8
/usr/lib/numad/numad-wrapper
/usr/lib/systemd/system/numad.service
/etc/numad.conf
/etc/logrotate.d/numad
```

This is intentional for compatibility with the existing daemon layout. The
`Makefile` install targets now match the service and init paths
(`${prefix}/lib/numad/numad-wrapper`), so `make install` produces a working
service.

`man numad` documents every option, including the startup-only long options.

## Key improvements over the original codebase

- `/proc` parsing hardened against sparse CPU numbering: all CPU-ID-indexed
  arrays and masks are sized by `cpu_id_limit` (highest possible CPU ID + 1,
  from `/sys/devices/system/cpu/present`) instead of the online CPU count, so
  offline/hotplugged CPUs cannot overflow the heap; `/proc/stat` lines with
  out-of-range CPU IDs are skipped
- correct parsing of process names from `/proc/<pid>/stat` (comm may contain
  spaces and parentheses)
- tracking process `start_time_ticks` to reduce PID reuse issues
- internal normalization around `node_ix` vs `node_id`
- bounded formatting of ID lists (no stack overflow on huge fragmented
  cpulists); oversized distance tables and zero-distance entries are
  sanitized instead of crashing or raising SIGFPE
- safer NUMA migration bitmask sizing and bounds checks before `SET_BIT`
- fixed candidate sorting logic
- explicit PID policy improvements (`-S 0` is honored by the GPU scanner too)
- stale GPU state cleanup and GPU admission thresholds
- rebind stability: cooldowns, anti-ping-pong, hysteresis, drift repair
- big-memory GPU processes are not squeezed onto the GPU node's CPUs
- migration-failure paths no longer drop the bind timestamp, so a persistent
  `migrate_pages` EPERM (e.g. inside a container) cannot cause a rebind loop

## Project status

This fork is focused on practical locality improvements and correctness
rather than large architectural rewrites.

## Origin

This project is based on the original numad codebase:

https://pagure.io/numad

## Support

Bitcoin: bc1qxk5ma6f82kwfcd6rznd73macchec8d83d70mpv
