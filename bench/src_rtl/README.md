# ReusePlan RTL Cost Models

This directory contains synthesizable SystemVerilog used to estimate the
incremental area and delay of ReusePlan-specific logic. It is not a complete CPU,
cache controller, or drop-in hardware implementation.

## Modules

| File | Purpose |
|---|---|
| `reuse_plan_victim_select.sv` | Combinational victim ranking and RRIP aging |
| `reuse_plan_replacement_path.sv` | Property/context qualification, epoch distance, static/online variant selection |
| `reuse_plan_online_selector.sv` | Five-arm leader/follower selection window |
| `reuse_bind_request_path.sv` | Request merge state, CSRs, sequence allocation, and sideband pipeline storage |
| `reuse_plan_recency_rank.sv` | Optional per-set recency-rank state |
| `reuse_plan_secded_49.sv` | SECDED encoding for 49 logical metadata bits |
| `tb_reuse_plan_*.sv` | Functional testbenches |

## Verification

Run:

```bash
python3 -m scripts.experiments.ecg.analysis.reuse_plan_rtl_verify
```

The command:

1. compiles and executes all three testbenches with Verilator;
2. elaborates each synthesis top with Yosys; and
3. runs Yosys structural checks.

The current modules pass these checks. The testbenches cover invalid-way
priority, all replacement variants, RRIP aging, circular epoch distance,
context qualification, online-window selection, request merging, sequence
allocation, recency rank, and SECDED single/double-error behavior.

## Scope

The RTL is suitable as a parameterized physical-cost input. It is not yet:

- integrated into an RTL RISC-V core or LLC;
- proven equivalent to the C++ policy for every possible cache state;
- formally verified;
- synthesized against the final technology library; or
- scaled using final machine-wide counts for MSHRs, harts, and pipeline copies.

The executable architecture remains the gem5/cache_sim/Sniper implementation.
Any published area, power, or timing number must come from a documented synthesis
flow using these modules and the final target parameters.
