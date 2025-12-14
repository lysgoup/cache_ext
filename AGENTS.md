# AGENTS.md - Guide for AI Coding Agents

## Prerequisites
- **Required kernel**: 6.6.8-cache-ext+ (check: `uname -r | grep -q "cache-ext"`)
- **C compiler**: clang-14 for BPF, standard clang for userspace
- **Permissions**: sudo required for policy loading and cgroup management

## Build Commands
- **All eBPF policies**: `./build_policies.sh` or `make -C policies -j`
- **Single policy**: `make -C policies cache_ext_lhd.out`
- **LevelDB**: `cd leveldb/build && cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build . -j`
- **My-YCSB**: `cd My-YCSB/build && cmake .. && make -j run_leveldb`
- **Clean policies**: `make -C policies clean`

## Test Commands
- **Quick policy test**: `cd policies && ./test_adaptive_v3.sh`
- **Workload test**: `cd policies && ./workload_test_v3.sh`
- **Full experiments**: `cd eval/<experiment> && ./run.sh` (ycsb, twitter, filesearch, isolation, get-scan, cpu-overhead)
- **Kernel verification**: All scripts check kernel automatically; manual check: `uname -r`

## Code Style - Python
- **Imports**: stdlib → third-party → local modules (separated by blank lines)
- **Type hints**: Use for parameters and returns: `def func(arg: str) -> int:`
- **Naming**: snake_case functions/variables, PascalCase classes
- **Logging**: Use `log.info/warning/error()`, not `print()`
- **Error handling**: Raise exceptions with descriptive messages

## Code Style - C/eBPF
- **Naming**: snake_case for all (functions, variables, structs)
- **BPF sections**: Use `SEC("struct_ops")` for policy hooks, `SEC("license")` for license
- **Headers**: Static inline helpers in `.bpf.h`, main logic in `.bpf.c`
- **Debugging**: `bpf_printk()` in BPF code, `fprintf(stderr, ...)` in userspace
- **Flags**: `-O2 -Wall` for both BPF and userspace; userspace uses `-fsanitize=address`

## Common Patterns
- **Policy lifecycle**: Python `CacheExtPolicy` class starts/stops userspace loaders
- **Cgroup management**: Policies watch directories via `--watch_dir`, attach to `--cgroup_path`
- **MGLRU toggle**: `./utils/enable-mglru.sh` or `./utils/disable-mglru.sh`
