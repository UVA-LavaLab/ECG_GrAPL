# ECG C++ Implementation

| Path | Purpose |
|---|---|
| `src/converter.cc` | Convert staged graph inputs to GAPBS `.sg` |
| `src_sim/` | Functional cache_sim ECG kernels and exact-policy tests |
| `src_gem5/` | gem5 ECG kernels and ISA decoder test |
| `src_sniper/` | Sniper ECG kernels, smokes, and canonical `sg_kernel` |
| `include/cache_sim/` | Functional cache hierarchy |
| `include/gem5_sim/` | gem5 overlays/configuration |
| `include/sniper_sim/` | Sniper overlays/configuration |
| `include/ecg_*` | Shared K2 record, mode-6, and victim-policy logic |

The GAPBS and reordering headers under `include/external/` and
`include/graphbrew/` are retained only as build dependencies for graph loading,
DBG order (`-o 5`), and P-OPT.

```bash
make all-sim
make gem5-riscv-m5ops-pr
make sniper-sg_kernel
```
