# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

cache_ext is a research system that enables custom page cache eviction policies using eBPF. It consists of a modified Linux kernel (v6.6.8), eBPF-based eviction policies, modified LevelDB, and My-YCSB benchmarking framework. This is an artifact evaluation repository for a SOSP 2025 paper.

## Prerequisites

This project is designed to run on Cloudlab c6525-25g instances with Ubuntu 22.04. The custom kernel must be installed and running before most components can be built or tested.

## Build and Installation Commands

### Initial Setup (one-time)

```bash
# Clone and initialize submodules
git submodule update --init --recursive

# Install and boot into custom kernel (REQUIRED FIRST)
./install_kernel.sh
# After completion, reboot into cache_ext kernel:
sudo grub-reboot "Advanced options for Ubuntu>Ubuntu, with Linux 6.6.8-cache-ext+"
sudo reboot now

# Download datasets (hundreds of GB, may take a while)
./download_dbs.sh

# Install remaining components (ONLY after booting into cache_ext kernel)
./install_filesearch.sh
./install_leveldb.sh
./install_ycsb.sh
./install_misc.sh
./setup_isolation.sh

# Build eBPF policies
./build_policies.sh
```

### Building Individual Components

```bash
# Rebuild eBPF policies (must be on cache_ext kernel)
make -C policies clean
make -C policies -j

# Rebuild LevelDB
cd leveldb/build
make clean
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j

# Rebuild My-YCSB
cd My-YCSB/build
make clean
cmake ..
make -j run_leveldb
```

### Kernel-Specific Checks

Most scripts verify the kernel before running:
```bash
uname -r | grep -q "cache-ext"  # Should succeed when on cache_ext kernel
```

## Running Experiments

Each experiment has a `run.sh` script in its subdirectory under `eval/`:

```bash
# Example: Run YCSB experiment (takes ~20 hours by default)
cd eval/ycsb
./run.sh

# Other experiments
cd eval/filesearch && ./run.sh
cd eval/isolation && ./run.sh
cd eval/get-scan && ./run.sh
cd eval/twitter && ./run.sh
cd eval/cpu-overhead && ./run.sh
```

Experiment iterations can be changed by modifying the `ITERATIONS` variable in each `run.sh` script. Results are saved to a top-level `results/` directory as JSON files.

Use `screen` for long-running experiments to avoid SSH disconnection issues.

## Plotting Results

```bash
cd bench
jupyter notebook
# Open bench_plot.ipynb
# Figures saved to figures/ directory as PDFs
```

## Code Architecture

### Four Major Components

1. **Modified Linux Kernel** (`linux/`): Linux v6.6.8 with cache_ext support, custom libbpf, and bpftool
2. **eBPF Policies** (`policies/`): Custom page cache eviction policies
3. **LevelDB** (`leveldb/`): Modified version on `cache_ext` branch
4. **My-YCSB** (`My-YCSB/`): C++ YCSB benchmarking framework

### Directory Structure

- `policies/`: eBPF policies (.bpf.c files) and userspace loaders (.c files)
  - Compiled into `.out` executables that load and manage eBPF programs
  - `cache_ext_lib.bpf.h`: Shared eBPF helpers and kfuncs
  - `dir_watcher.bpf.h`: Directory monitoring functionality
  - Policy implementations: LHD, S3-FIFO, FIFO, MRU, MGLRU, sampling, GET-SCAN
- `bench/`: Python benchmarking framework
  - `bench_lib.py`: Core library with `CacheExtPolicy` class and utilities
  - `bench_leveldb.py`, `bench_fio.py`, etc.: Specific benchmark implementations
  - `bench_plot.ipynb`: Jupyter notebook for generating figures
- `eval/`: Experiment scripts, each with `run.sh` and experiment-specific configuration
- `utils/`: Helper scripts for MGLRU toggling (`enable-mglru.sh`, `disable-mglru.sh`)

### eBPF Policy Architecture

Policies consist of two parts:
1. **eBPF kernel code** (`.bpf.c`): Implements eviction logic using BPF struct_ops, runs in kernel
2. **Userspace loader** (`.c`): Loads BPF program, manages cgroups, handles directory watching

The `CacheExtPolicy` Python class (`bench/bench_lib.py`) manages policy lifecycle:
- Starts userspace loader with `--watch_dir` and `--cgroup_path` parameters
- Optionally sets `--cgroup_size` for per-cgroup limits
- Policies interact with kernel via BPF maps and kfuncs defined in `cache_ext_lib.bpf.h`

### Benchmarking Framework

Python scripts in `bench/` orchestrate experiments:
- Manage cgroups and memory limits
- Start/stop eBPF policies via `CacheExtPolicy`
- Run workloads (LevelDB, fio, ripgrep, etc.)
- Parse results and output JSON
- Support both baseline (no policy) and cache_ext modes

### MGLRU (Multi-Gen LRU)

The kernel supports toggling between standard LRU and MGLRU:
- Enable: `./utils/enable-mglru.sh` (writes 'y' to `/sys/kernel/mm/lru_gen/enabled`)
- Disable: `./utils/disable-mglru.sh` (writes 'n')
- Some experiments compare cache_ext policies against MGLRU baseline

### Integration Points

- **LevelDB**: Uses custom `cache_ext` branch with minor modifications
- **My-YCSB**: Multiple database backends (LevelDB, RocksDB, Redis, WiredTiger)
- **Datasets**: Downloaded to `/mydata/leveldb_db_orig/` and similar paths via `download_dbs.sh`
- **Results**: All experiments write to `results/*.json` at repository root

## Important Notes

- Kernel version check is ubiquitous - most operations fail gracefully if not on cache_ext kernel
- Policies are built with clang-14 and require bpftool from custom kernel installation
- The repository expects to be in `/mydata/cache_ext/` on Cloudlab but should work elsewhere
- Some experiments require ~500GB disk space due to large datasets
- Per-cgroup isolation experiments use separate cgroup directories cloned at `../cgroup/`
