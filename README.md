`amogus-numa-daemon` is a fork of `numad`: a Linux NUMA daemon that monitors system topology and workload placement, then improves CPU and memory locality to reduce unnecessary remote access costs.

This fork extends the original idea with a stronger focus on modern multi-socket and heterogeneous systems, including:

- safer and more robust `/proc` parsing
- corrected internal NUMA node handling
- GPU-aware locality support
- compatibility with `sched_ext`-based schedulers
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

It does **not** move to a cgroup-first or service-first model. Instead, it continues to:

- inspect processes through `/proc`
- compute their effective load and locality needs
- choose preferred NUMA nodes
- set CPU affinity
- optionally migrate memory when that is actually beneficial

Explicitly included PIDs are treated as always manageable.

## AMD GPU awareness

This fork adds GPU-aware placement logic for AMD GPU systems.

### Implemented today

- GPU topology discovery through sysfs / DRM
- GPU-local CPU narrowing using device `local_cpus`
- GPU-aware NUMA node preference
- switchable graphics placement policy: `auto`, `prefer`, or `strict`
- DRM `fdinfo`-based process activity tracking
- stale GPU state aging and cleanup
- safer handling of low-RSS but GPU-significant processes

### Backend notes

The currently implemented process telemetry path is based on **DRM `fdinfo`**.

The `amdsmi` backend option is currently treated as a warning/fallback path rather than a fully implemented collector.

### Graphics placement policy

`amogus-numa-daemon` now exposes `--gpu-graphics-placement=auto|prefer|strict` for graphics-oriented GPU workloads.

- `auto` keeps the historical behavior: try GPU-local NUMA nodes first, then fall back to generic NUMA placement when needed.
- `prefer` is the explicit soft mode: GUI / graphics processes prefer GPU-local NUMA nodes, but fallback remains allowed.
- `strict` is the hard mode: when a graphics process has valid GPU-local NUMA node information, fallback outside those nodes is refused.

This policy only affects node selection. Memory migration remains controlled separately by `--gpu-migrate`.
With `--gpu-migrate=auto`, pure graphics workloads still avoid aggressive page migration; the daemon now logs CPU affinity changes and memory migration decisions separately so this is visible in the log.

## `sched_ext` compatibility

This project is compatible with `sched_ext`

- detects whether `sched_ext` is enabled
- detects which scheduler is active
- cooperates by constraining allowed CPU affinity masks

This means the daemon can work correctly alongside `sched_ext` schedulers without conflicting with them.

### Recommended schedulers

For NUMA systems, the preferred schedulers are:

- `scx_beerland`
- `scx_p2dq`

### Generally not recommended for this use case

For gaming, AI inference, and other tasks, latency is completely irrelevant. Especially for large NUMA systems, ensuring data locality is far more important; therefore, these schedulers are generally NOT RECOMMENDED:

- `scx_lavd`
- `scx_bpfland`
- `scx_rustland`
- `scx_flash`

`scx_rusty` is also not the preferred first choice for multi-socket NUMA systems.

## Key improvements over the original codebase

This fork includes a number of correctness and hardening fixes, including:

- safer `/proc` parsing
- correct parsing of process names from `/proc/<pid>/stat`
- tracking process `start_time_ticks` to reduce PID reuse issues
- internal normalization around `node_ix` vs `node_id`
- safer NUMA migration bitmask sizing
- fixed candidate sorting logic
- explicit PID policy improvements
- stale GPU state cleanup
- zero-safe handling for low-memory GPU-first workloads


## Configuration

The daemon reads configuration from:

/etc/numad.conf

Example service wrapper usage is based on:

/usr/lib/numad/numad-wrapper
Running with systemd

A systemd unit file is included:

numad.service

## Typical usage:

sudo systemctl daemon-reload
sudo systemctl enable --now numad.service

## Packaging

An Arch PKGBUILD is included in the repository.

The package keeps the historical installed file names such as:

/usr/bin/numad
/etc/numad.conf
/usr/lib/systemd/system/numad.service

This is intentional for compatibility with the existing daemon layout.

Legacy init script

A SysV-style init script is also included:

numad.init

It is mainly useful for non-systemd environments that still provide compatible init helper functions.

## Project status

This fork is focused on practical locality improvements and correctness rather than large architectural rewrites.

## Origin

This project is based on the original numad codebase:

https://pagure.io/numad

## Support
Bitcoin: bc1qxk5ma6f82kwfcd6rznd73macchec8d83d70mpv
