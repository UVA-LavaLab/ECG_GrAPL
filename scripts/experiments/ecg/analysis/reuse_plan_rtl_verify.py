#!/usr/bin/env python3
"""Require and execute generic functional/synthesis checks for ReusePlan RTL inputs."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Any

from scripts.experiments.ecg.analysis.reuse_plan_cacti_packet import sha256_file
from scripts.experiments.ecg.analysis.reuse_plan_rtl_packet import (
    ECC_RTL,
    ONLINE_RTL,
    RECENCY_RTL,
    REPLACEMENT_RTL,
    REQUEST_RTL,
    RTL_ROOT,
    TESTBENCH,
    REPLACEMENT_TESTBENCH,
    REQUEST_TESTBENCH,
    VICTIM_RTL,
)


def required_tool(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise RuntimeError(f"required RTL verification tool is missing: {name}")
    return path


def tool_version(path: str) -> str:
    result = subprocess.run(
        [path, "--version"], check=True, capture_output=True, text=True)
    return (result.stdout or result.stderr).splitlines()[0].strip()


def verify(work_dir: Path) -> dict[str, Any]:
    verilator = required_tool("verilator")
    yosys = required_tool("yosys")
    work_dir.mkdir(parents=True, exist_ok=True)
    simulations = (
        (
            "tb_reuse_plan_physical_logic",
            [VICTIM_RTL, ECC_RTL, TESTBENCH],
            "ReusePlan physical RTL tests passed",
        ),
        (
            "tb_reuse_plan_replacement_path",
            [VICTIM_RTL, ONLINE_RTL, REPLACEMENT_RTL,
             REPLACEMENT_TESTBENCH],
            "ReusePlan replacement path tests passed",
        ),
        (
            "tb_reuse_bind_request_path",
            [REQUEST_RTL, RECENCY_RTL, REQUEST_TESTBENCH],
            "ReusePlan request path tests passed",
        ),
    )
    for top, sources, marker in simulations:
        obj_dir = work_dir / top
        subprocess.run([
            verilator,
            "--binary",
            "--timing",
            "-Wall",
            "-Wno-DECLFILENAME",
            "-Wno-UNUSEDSIGNAL",
            "--top-module", top,
            "--Mdir", str(obj_dir),
            *(str(source) for source in sources),
        ], check=True, capture_output=True, text=True)
        simulation = subprocess.run(
            [str(obj_dir / f"V{top}")],
            check=True, capture_output=True, text=True)
        if marker not in simulation.stdout:
            raise RuntimeError(
                f"{top} functional test did not report success")

    for top, sources in (
            ("reuse_plan_victim_select", [VICTIM_RTL]),
            ("reuse_plan_secded_49_parallel16", [ECC_RTL]),
            ("reuse_plan_replacement_path",
             [VICTIM_RTL, ONLINE_RTL, REPLACEMENT_RTL]),
            ("reuse_bind_request_state_slot", [REQUEST_RTL]),
            ("reuse_plan_csr_state", [REQUEST_RTL]),
            ("reuse_plan_sequence_allocator", [REQUEST_RTL]),
            ("reuse_bind_request_pipeline_stage", [REQUEST_RTL]),
            ("reuse_plan_recency_rank_state", [RECENCY_RTL])):
        script = (
            "read_verilog -sv " +
            " ".join(str(source) for source in sources) +
            f"; hierarchy -check -top {top}; proc; opt; check -assert")
        subprocess.run(
            [yosys, "-q", "-p", script],
            check=True, capture_output=True, text=True)

    return {
        "status": "passed",
        "verilator": tool_version(verilator),
        "yosys": tool_version(yosys),
        "inputs": {
            str(path.relative_to(RTL_ROOT.parent.parent)): sha256_file(path)
            for path in (
                VICTIM_RTL, ECC_RTL, ONLINE_RTL, REPLACEMENT_RTL,
                REQUEST_RTL, RECENCY_RTL, TESTBENCH,
                REPLACEMENT_TESTBENCH, REQUEST_TESTBENCH)
        },
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run required generic checks for ReusePlan physical RTL.")
    parser.add_argument("--work-dir", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.work_dir:
        payload = verify(args.work_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="reuse_plan-rtl-verify-") as temp:
            payload = verify(Path(temp))
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
