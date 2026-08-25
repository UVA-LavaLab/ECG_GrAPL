#!/usr/bin/env python3
"""Reproducible correctness check for the L3 replacement policies.

Runs each policy on a tiny controlled cell with ECG_EVICT_TRACE enabled, parses
the per-eviction trace (each way's rrpv/epoch/dist/property/recency + the chosen
victim), and ASSERTS the victim matches the policy's defining rule. Exit code 0
iff every eviction of every policy obeys its spec. Researcher-runnable artifact
verification — no trust in aggregate numbers required.

Cross-sim equivalence argument (what guarantees cache_sim == gem5 == Sniper):
  1. SHARED DECISION: all three call the same ecg_policy::selectVictim
     (ecg_victim_policy.h). The synthetic test (test_ecg_victim.cc) pins its
     EXACT victim for controlled sets — strict + mutation-proven, incl. the
     valid-bit / wraparound / fallback corner cases.
  2. SHARED ISA LAYOUT: all three (and the gem5 RISC-V decoder) include the same
     ecg_mode6_builder.h pack/extract; test_ecg_packed_field_parity.cc pins it.
  3. PER-SIM ADAPTER: each backend's native-state -> WayState mapping is covered
     by its LIVE trace obeying spec (verify_trace, run with --gem5 / --sniper)
     PLUS the record-never-stamped invariant (records must never carry a stamp).
  4. STAMPING PATH: BC + per-edge masks exercises the clearEdgeEpoch delivery-vs-
     cleared path live (the over-stamping bug locus that per-sim spec checks alone
     cannot catch); OMP=4 re-checks it under the per-thread hint path.
  Residual: the epoch VALUE rarely discriminates live on gem5/Sniper (no
  ECG_STORED_REFRESH there), so their epoch-coverage is INFORMATIONAL; the strict
  epoch-discrimination guarantee comes from (1) + the cache_sim mirror.

  python3 scripts/experiments/ecg/verify/ecg.py            # cache_sim (+ synthetic)
  python3 scripts/experiments/ecg/verify/ecg.py --gem5     # + gem5 adapter traces
  python3 scripts/experiments/ecg/verify/ecg.py --sniper   # + Sniper adapter traces
"""

import hashlib, json, os, re, subprocess, sys, tempfile
from datetime import datetime, timezone
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from equiv import check_insertion_rrpv_invariant, check_behavioral_equivalence  # noqa: E402

ROOT = Path(__file__).resolve().parents[4]
PR = ROOT / "bench" / "bin_sim" / "pr"
GRAPH = ROOT / "results" / "graphs" / "email-Eu-core" / "email-Eu-core.sg"
GRAPH_ARGS = (
    ["-f", str(GRAPH)] if GRAPH.exists()
    else ["-g", "12", "-k", "16"])
GRAPH_OPTIONS = (
    f"-f {GRAPH}" if GRAPH.exists()
    else "-g 12 -k 16")
GRAPH_LABEL = "email-Eu-core" if GRAPH.exists() else "synthetic-g12"

BASE_ENV = dict(CACHE_ULTRAFAST="0", CACHE_L1_POLICY="LRU", CACHE_L2_POLICY="LRU",
                CACHE_L1_SIZE="2kB", CACHE_L1_WAYS="8", CACHE_L2_SIZE="4kB",
                CACHE_L2_WAYS="8", CACHE_L3_SIZE="16kB", CACHE_L3_WAYS="8",
                CACHE_LINE_SIZE="64", OMP_NUM_THREADS="1", ECG_EVICT_TRACE="40")
ECG_ENV = dict(CACHE_POLICY="ECG", CACHE_L3_POLICY="ECG", ECG_MODE="ECG_GRASP_POPT",
               ECG_EXACT_REREF="1", ECG_PREFETCH_MODE="6", ECG_EDGE_MASK_EPOCH="1",
               ECG_EDGE_MASK_LINEMIN="1", ECG_EDGE_MASK_EPOCHS="65535",
               ECG_EDGE_MASK_LEAN="1", ECG_EDGE_MASK_PACK="1", ECG_EDGE_MASK_CHARGED="1")
# Epoch-coverage geometry: a big L2 absorbs the edge stream so the L3 sees
# property-dominated sets (the default workload only ever evicts records, so the
# epoch-property branch never fires); ECG_STORED_REFRESH broadcasts the next-ref
# epoch to the L3 so resident property lines carry a live stamp and the epoch
# VALUE actually discriminates between competing property lines. This is how the
# core ECG eviction logic is exercised end-to-end on the real simulator code.
COV_ENV = dict(CACHE_L2_SIZE="1MB", CACHE_L3_SIZE="4kB", ECG_STORED_REFRESH="1",
               ECG_EVICT_TRACE="4000")

WAY_RE = re.compile(
    r"way(\d+) valid=(\d+) rrpv=(\d+) epoch=(\d+) dist=(\d+) "
    r"prop=(\d+) stamped=(\d+)(?: dbg=(\d+))? last=(\d+)"
    r"(?: epoch2=(\d+) sched_n=(\d+))?"
)
HDR_RE = re.compile(r"\[EVICT L3 pol=(\S+)")
REUSE_PLAN_HDR_RE = re.compile(r"\[EVICT L3 .*curEpoch=(\d+)")
REUSE_PLAN_DELIVERY_RE = re.compile(
    r"\[ECG-ReusePlan-(EXPECT|RECV|SIDEBAND) sim=(\w+) seq=(\d+) "
    r"dest=(\d+) tier=(\d+) epoch1=(\d+) epoch2=(\d+)"
    r"(?: width=(\d+))?\]")
REUSE_PLAN_ACCEPT_RE = re.compile(
    r"\[ECG-ReuseBind-ACCEPT sim=gem5 seq=(\d+) "
    r"request_seq=(\d+) request_dest=(\d+) fill_dest=(\d+) "
    r"source=(request|mailbox) "
    r"tier=(\d+) epoch1=(\d+) epoch2=(\d+) current=(\d+) "
    r"context=(\d+) (?:property_elem_bytes|width)=(\d+)\]")
REUSE_PLAN_REQUEST_RE = re.compile(
    r"\[ECG-ReuseBind-REQUEST sim=gem5 seq=(\d+) request_seq=(\d+) "
    r"dest=(\d+) tier=(\d+) epoch1=(\d+) epoch2=(\d+) "
    r"current=(\d+) context=(\d+)\]")
REUSE_PLAN_FUSED_RECV_RE = re.compile(
    r"\[ECG-ReusePlan-FUSED-RECV sim=sniper seq=(\d+) src=(\d+) "
    r"line=(\d+) addr_line=0x([0-9a-fA-F]+) vpl=(\d+) "
    r"index=(\d+) begin=(\d+) end=(\d+) "
    r"dest=(\d+) tier=(\d+) epoch1=(\d+) epoch2=(\d+)\]")
REUSE_PLAN_BIND_CONSUME_RE = re.compile(
    r"\[ECG-ReusePlan-BIND-CONSUME sim=sniper seq=(\d+) core=(\d+) "
    r"bound=0x([0-9a-fA-F]+) line=0x([0-9a-fA-F]+) size=(\d+) "
    r"current=(\d+) context=(\d+)\]")
REUSE_PLAN_FUSED_VALID_RE = re.compile(
    r"\[ECG-ReusePlan-FUSED-VALID count=(\d+) bad=(\d+)\]")
VIC_RE = re.compile(r"-> victim=way(\d+)(?: reason=(.*))?")


def run(policy_env, extra=None):
    env = {**os.environ, **BASE_ENV, **policy_env, **(extra or {})}
    p = subprocess.run([str(PR), *GRAPH_ARGS, "-o", "0", "-n", "1", "-i", "1"],
                       env=env, capture_output=True, text=True, timeout=300)
    return p.stderr, (p.returncode == 0)


BC = ROOT / "bench" / "bin_sim" / "bc"


def run_bc(policy_env, extra=None):
    """Run BC instead of PR. BC's bottom-up + back-propagation traversal NATURALLY
    evicts PROPERTY lines (farthest-epoch branch), which the PR workload never does
    (PR only ever evicts records) — so this is the live cross-kernel coverage of the
    epoch-eviction path, on a different adapter access pattern than PR."""
    env = {**os.environ, **BASE_ENV, **policy_env, **(extra or {})}
    p = subprocess.run([str(BC), *GRAPH_ARGS, "-o", "0", "-n", "1"],
                       env=env, capture_output=True, text=True, timeout=300)
    return p.stderr, (p.returncode == 0)


GEM5_OPT = ROOT / "bench" / "include" / "gem5_sim" / "gem5" / "build" / "RISCV" / "gem5.opt"
ROI_MATRIX = ROOT / "scripts" / "experiments" / "ecg" / "roi_matrix.py"


def run_gem5_isa_modes(receipt_dir=None):
    """Run EVERY consolidated ecg.load (mode x dest-width) through the REAL gem5 RISC-V
    DECODER and verify the decoded dest (rd = prop[dest], prop[i]=i). The field-parity
    test only checks a C++ MIRROR of the decoder shifts and the 3-sim verify exercises
    eviction via the X86 m5op path, so NEITHER runs the actual decoded ecg.load for the
    new modes/widths — this does. Includes a TEETH proof: forcing the EMITTED width wrong
    (ECG_TEST_FORCE_WC) while the record is packed correctly MUST make the decoder extract
    a different dest -> FAIL, proving ECG_WIDTH is load-bearing (the test is not vacuous)."""
    gem5_dir = ROOT / "bench" / "include" / "gem5_sim" / "gem5"
    se = gem5_dir / "configs" / "deprecated" / "example" / "se.py"
    binp = ROOT / "bench" / "bin_gem5" / "test_ecg_load_modes_riscv_m5ops"
    if not binp.exists():
        subprocess.run(["make", "bench/bin_gem5/test_ecg_load_modes_riscv_m5ops"],
                       cwd=str(ROOT), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    if not binp.exists():
        print("  gem5 ISA decode: [FAIL] could not build test_ecg_load_modes"); return False

    atomic_runs = []

    def _run(label, env_file=None, guest_options=None):
        cmd = [str(GEM5_OPT), "--outdir=/tmp/ecg_modes_verify", str(se),
               "--cmd", str(binp), "--cpu-type=AtomicSimpleCPU"]
        if env_file:
            cmd += ["--env", env_file]
        if guest_options:
            cmd += ["--options", guest_options]
        env = {
            "PATH": "/usr/bin:/bin",
            "HOME": "/tmp",
            "TMPDIR": "/tmp",
            "LC_ALL": "C",
            "LANG": "C",
        }
        try:
            p = subprocess.run(
                cmd, cwd=str(gem5_dir), env=env,
                capture_output=True, text=True, timeout=300)
            text = p.stdout + p.stderr
            atomic_runs.append({
                "label": label,
                "command": cmd,
                "environment": env,
                "env_file": (
                    Path(env_file).read_text(errors="ignore")
                    if env_file else ""),
                "guest_options": guest_options or "",
                "exit_code": p.returncode,
                "timed_out": False,
            })
        except subprocess.TimeoutExpired:
            text = ""
            atomic_runs.append({
                "label": label,
                "command": cmd,
                "environment": env,
                "env_file": (
                    Path(env_file).read_text(errors="ignore")
                    if env_file else ""),
                "guest_options": guest_options or "",
                "exit_code": None,
                "timed_out": True,
            })
        return text

    normal = _run("atomic_normal")
    normal_process_pass = (
        atomic_runs[-1]["exit_code"] == 0 and
        atomic_runs[-1]["timed_out"] is False)
    plan_flow_load_pass = "PLAN/FLOW record=" in normal and "stream=" in normal
    compact_stream_reuse_bind_pass = (
        "ReuseBind-Compact-Flow" in normal and
        "canonical=0x3a004700000025" in normal)
    weighted_load_pass = (
        "PLAN/FLOW weighted sidecar=" in normal and "stream=" in normal)
    indexed_bind_pass = (
        "ReuseBind-Indexed-U32" in normal and "ReuseBind-Indexed-CW24" in normal)
    computed_address_pass = all(marker in normal for marker in (
        "ReuseBind-U32", "ReuseBind-U32-HIGH", "ReuseBind-S32", "ReuseBind-F32",
        "mrd=0x123456789abcdef0", "mrd=0x76543210"))
    normal_pass = (
        normal_process_pass and "RESULT: PASS" in normal and
        plan_flow_load_pass and
        compact_stream_reuse_bind_pass and weighted_load_pass and
        indexed_bind_pass and computed_address_pass)

    graph_se = (
        ROOT / "bench" / "include" / "gem5_sim" / "configs" /
        "graphbrew" / "graph_se.py")
    if not graph_se.exists():
        print(f"  gem5 compact proposal O3 probe: [FAIL] missing {graph_se}")
        return False
    with tempfile.TemporaryDirectory(
            prefix="ecg_modes_verify_o3_") as temp_dir:
        outdir = Path(temp_dir)
        context = outdir / "context.json"
        o3_env = {
            "PATH": "/usr/bin:/bin",
            "HOME": temp_dir,
            "TMPDIR": temp_dir,
            "LC_ALL": "C",
            "LANG": "C",
            "ECG_REUSE_PLAN_DEPTH": "2",
            "ECG_VARIANT": "epoch_first",
            "ECG_FLOWTHROUGH": "1",
            "ECG_FLOWTHROUGH_TRACE": "8",
            "ECG_REUSE_PLAN_DELIVERY_TRACE": "8",
            "GEM5_ECG_PRODUCER": "1",
            "GEM5_ECG_FLOWTHROUGH_REQUEST_BOUND": "1",
            "GEM5_FORCE_ECG_PLOAD": "1",
            "GEM5_FORCE_ECG_FLOW_LOAD": "1",
            "GEM5_ECG_ISA_VARIANT": "computed",
            "GEM5_ECG_EPOCH_REGION_INDEX": "0",
            "GEM5_GRAPHBREW_CTX": str(context),
        }
        o3_cmd = [
            str(GEM5_OPT), f"--outdir={outdir}", str(graph_se),
            "--binary", str(binp), "--options", "proposal-only",
            "--cpu-type", "O3", "--policy", "ECG",
            "--ecg-mode", "ECG_GRASP_POPT", "--prefetcher", "none",
            "--l1d-size", "4kB", "--l1i-size", "4kB",
            "--l2-size", "8kB", "--l3-size", "16kB",
            "--l3-ways", "8",
        ]
        try:
            o3_result = subprocess.run(
                o3_cmd, cwd=str(gem5_dir), env=o3_env,
                capture_output=True, text=True, timeout=300)
            o3_text = o3_result.stdout + o3_result.stderr
            o3_exit_code = o3_result.returncode
            o3_timed_out = False
        except subprocess.TimeoutExpired:
            o3_result = None
            o3_text = ""
            o3_exit_code = None
            o3_timed_out = True
        for output_name in (
                "benchmark_stdout.txt", "benchmark_stderr.txt"):
            output_path = outdir / output_name
            if output_path.exists():
                o3_text += "\n" + output_path.read_text(errors="ignore")

    expected_payload = (37, 3, 17, 29, 11, 7)
    request_by_sequence = {}
    request_conflict = False
    for match in REUSE_PLAN_REQUEST_RE.finditer(o3_text):
        values = tuple(map(int, match.groups()))
        request_sequence = values[1]
        payload = values[2:]
        previous = request_by_sequence.setdefault(
            request_sequence, payload)
        request_conflict |= previous != payload
    exact_request_sequences = {
        sequence for sequence, payload in request_by_sequence.items()
        if payload == expected_payload
    }
    exact_accept = False
    for match in REUSE_PLAN_ACCEPT_RE.finditer(o3_text):
        values = match.groups()
        request_sequence = int(values[1])
        request_dest = int(values[2])
        fill_dest = int(values[3])
        source = values[4]
        payload = tuple(map(int, values[5:10]))
        property_elem_bytes = int(values[10])
        same_line = (
            (request_dest * property_elem_bytes) // 64 ==
            (fill_dest * property_elem_bytes) // 64)
        if (
            request_sequence in exact_request_sequences and
            request_dest == expected_payload[0] and same_line and
            source == "request" and
            payload == expected_payload[1:] and
            property_elem_bytes == 4
        ):
            exact_accept = True
            break
    compact_request_bound_pass = (
        o3_result is not None and o3_result.returncode == 0 and
        re.search(r"ReuseBind-Compact-Flow[^\n]*\[OK\]", o3_text) is not None and
        "[test_ecg_load_modes] RESULT: PASS" in o3_text and
        "canonical=0x3a004700000025" in o3_text and
        not request_conflict and
        bool(exact_request_sequences) and exact_accept)
    compact_request_flowthrough_pass = bool(re.search(
        r"\[ECG-FLOWTHROUGH sim=gem5 [^\n]*"
        r"size=4 [^\n]*source=request-flag [^\n]*allocate=0\]",
        o3_text))

    ef = Path("/tmp") / "ecg_force_wc0.env"
    ef.write_text("ECG_TEST_FORCE_WC=0\n")
    teeth = _run("atomic_teeth", str(ef))
    teeth_fail = (
        atomic_runs[-1]["exit_code"] == 0 and
        atomic_runs[-1]["timed_out"] is False and
        "RESULT: FAIL" in teeth)
    proposal_teeth = _run(
        "atomic_proposal_format_teeth",
        guest_options="proposal-wrong-format")
    proposal_format_teeth_fail = (
        atomic_runs[-1]["exit_code"] == 0 and
        atomic_runs[-1]["timed_out"] is False and
        re.search(
            r"ReuseBind-Compact-Flow[^\n]*\[FAIL\]",
            proposal_teeth) is not None and
        "[test_ecg_load_modes] RESULT: FAIL" in proposal_teeth)
    print(f"  gem5 ISA decode every (mode,width) via REAL decoder -> PASS: "
          f"{'[OK ]' if normal_pass else '[FAIL]'}")
    print(f"  gem5 ReusePlan/FlowThrough record-load round-trip: "
          f"{'[OK ]' if plan_flow_load_pass else '[FAIL]'}")
    print(f"  gem5 compact FlowThrough decoder -> canonical ReuseBind metadata: "
          f"{'[OK ]' if compact_stream_reuse_bind_pass else '[FAIL]'}")
    print(f"  gem5 compact FlowThrough request-flag FlowThrough: "
          f"{'[OK ]' if compact_request_flowthrough_pass else '[FAIL]'}")
    print(f"  gem5 compact ReuseBind exact O3 Request metadata: "
          f"{'[OK ]' if compact_request_bound_pass else '[FAIL]'}")
    print(f"  gem5 weighted 4B ReusePlan/FlowThrough round-trip: "
          f"{'[OK ]' if weighted_load_pass else '[FAIL]'}")
    print(f"  gem5 indexed ReuseBind property-load round-trip: "
          f"{'[OK ]' if indexed_bind_pass else '[FAIL]'}")
    print(f"  gem5 computed-address ReuseBind typed-load round-trip: "
          f"{'[OK ]' if computed_address_pass else '[FAIL]'}")
    print(f"  gem5 ISA teeth (forced-wrong ECG_WIDTH must mis-decode -> FAIL): "
          f"{'[OK ]' if teeth_fail else '[FAIL]'}")
    print(f"  gem5 compact proposal teeth (wrong record-format CSR -> FAIL): "
          f"{'[OK ]' if proposal_format_teeth_fail else '[FAIL]'}")
    overall_pass = (
        normal_pass and compact_request_bound_pass and
        compact_request_flowthrough_pass and teeth_fail and
        proposal_format_teeth_fail)
    if receipt_dir:
        receipt_path = Path(receipt_dir)
        receipt_path.mkdir(parents=True, exist_ok=True)
        outputs = {
            "atomic_normal.log": normal,
            "atomic_teeth.log": teeth,
            "atomic_proposal_format_teeth.log": proposal_teeth,
            "o3_proposal.log": o3_text,
        }
        for name, text in outputs.items():
            (receipt_path / name).write_text(text)

        def descriptor(path):
            data = Path(path).read_bytes()
            return {
                "path": str(Path(path).resolve()),
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
                "rows": None,
            }

        payload = {
            "schema": "graphbrew-ecg-reuse_bind-decoder-probe-v1",
            "created_utc": datetime.now(timezone.utc).isoformat(
                timespec="seconds"),
            "overall_pass": overall_pass,
            "expected": {
                "canonical": "0x3a004700000025",
                "payload": list(expected_payload),
                "property_value_bits": "0x41234567",
                "record_request_bytes": 4,
            },
            "checks": {
                "atomic_all_modes": normal_pass,
                "atomic_flow_load": stream_load_pass,
                "atomic_compact_stream_to_reuse_bind": compact_stream_reuse_bind_pass,
                "atomic_weighted_plan_load": weighted_load_pass,
                "atomic_masked_property_load": masked_pload_pass,
                "atomic_typed_reuse_bind": computed_address_pass,
                "atomic_wrong_width_teeth": teeth_fail,
                "atomic_proposal_wrong_format_teeth":
                    proposal_format_teeth_fail,
                "o3_exact_request_binding": compact_request_bound_pass,
                "o3_request_flag_flowthrough": compact_request_flowthrough_pass,
            },
            "runs": {
                "atomic": atomic_runs,
                "o3": {
                    "command": o3_cmd,
                    "environment": o3_env,
                    "exit_code": o3_exit_code,
                    "timed_out": o3_timed_out,
                },
            },
            "inputs": {
                "gem5_opt": descriptor(GEM5_OPT),
                "guest": descriptor(binp),
                "atomic_config": descriptor(se),
                "o3_config": descriptor(graph_se),
                "verifier": descriptor(Path(__file__)),
                "decoder_overlay": descriptor(
                    ROOT / "bench" / "include" / "gem5_sim" / "overlays" /
                    "arch" / "riscv" / "isa" /
                    "decoder_ecg_extract.isa"),
            },
            "outputs": {
                name: descriptor(receipt_path / name)
                for name in outputs
            },
        }
        (receipt_path / "decoder_probe_receipt.json").write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n")
    return overall_pass


def run_gem5(variant, cov=False):
    """Run gem5 ECG_GRASP_POPT on the tiny graph with the trace on; return the
    gem5 log text (run_command pipes the policy's stderr trace into the log).
    cov=True uses the epoch-coverage geometry (big L2 + small L3 + STORED_REFRESH)
    so the property-eviction / epoch branch is exercised."""
    out = Path("/tmp") / f"verify_gem5_{variant}{'_cov' if cov else ''}"
    env = {**os.environ, "GEM5_OPT": str(GEM5_OPT), "GEM5_KERNEL_SUFFIX": "_riscv_m5ops",
           "GEM5_FORCE_ECG_EXTRACT": "1", "GEM5_ECG_PFX_MODE": "6", "ECG_PREFETCH_MODE": "6",
           "ECG_VARIANT": variant, "ECG_EVICT_TRACE": "4000" if cov else "40"}
    l3, l2 = ("4kB", "1MB") if cov else ("16kB", "4kB")
    if cov:
        env["ECG_STORED_REFRESH"] = "1"
    cmd = [sys.executable, str(ROI_MATRIX), "--suite", "gem5", "--no-build",
           "--benchmark", "pr", "--policies", "ECG:ECG_GRASP_POPT",
           "--options", f"{GRAPH_OPTIONS} -o 5 -n 1 -i 1",
           "--l3-sizes", l3, "--l3-ways", "8", "--l1d-size", "2kB", "--l2-size", l2,
           "--out-dir", str(out)]
    subprocess.run(cmd, env=env, cwd=str(ROOT),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900, check=False)
    logs = sorted((out / "logs").glob("*.log")) if (out / "logs").exists() else []
    text = logs[0].read_text(errors="ignore") if logs else ""
    return text, bool(text)


def run_sniper(variant):
    """Run Sniper ECG_GRASP_POPT on the tiny graph with the trace on; return the
    Sniper log text. The sg_kernel workload is gated (Sniper/SDE has a documented
    ~50 GiB runaway), so it runs under prlimit via --sniper-memory-limit-gb."""
    import shutil
    out = Path("/tmp") / f"verify_sniper_{variant}"
    shutil.rmtree(out, ignore_errors=True)
    env = {**os.environ, "SNIPER_ECG_MODE": "ECG_GRASP_POPT",
           "ECG_VARIANT": variant, "ECG_EVICT_TRACE": "40"}
    cmd = [sys.executable, str(ROI_MATRIX), "--suite", "sniper",
           "--sniper-workload", "sg_kernel", "--allow-sniper-sg-kernel-workload",
           "--sniper-memory-limit-gb", "20", "--sniper-enable-graph-policies",
           "--no-build", "--benchmark", "pr", "--policies", "ECG:ECG_GRASP_POPT",
           "--options", f"{GRAPH_OPTIONS} -o 5 -n 1 -i 1",
           "--l3-sizes", "16kB", "--l3-ways", "8", "--l1d-size", "2kB", "--l2-size", "4kB",
           "--timeout-sniper", "540", "--out-dir", str(out)]
    subprocess.run(cmd, env=env, cwd=str(ROOT),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900, check=False)
    logs = sorted((out / "logs").glob("*.log")) if (out / "logs").exists() else []
    text = logs[0].read_text(errors="ignore") if logs else ""
    return text, bool(text)


def parse_blocks(text):
    """Yield (pol, ways[list of dict], victim_way, reason)."""
    pol = None; ways = []; victim = None; reason = None
    for line in text.splitlines():
        h = HDR_RE.search(line)
        if h:
            if pol and ways: yield pol, ways, victim, reason
            pol = h.group(1); ways = []; victim = None; reason = None; continue
        m = WAY_RE.search(line)
        if m:
            groups = m.groups()
            w, valid, rrpv, epoch, dist, prop, stamped = map(int, groups[:7])
            dbg = int(groups[7]) if groups[7] is not None else 0
            last = int(groups[8])
            epoch2 = int(groups[9]) if groups[9] is not None else epoch
            sched_n = int(groups[10]) if groups[10] is not None else 1
            ways.append(dict(way=w, valid=valid, rrpv=rrpv, epoch=epoch,
                             dist=dist, prop=prop, stamped=stamped,
                             dbg=dbg, last=last, epoch2=epoch2,
                             sched_n=sched_n))
        v = VIC_RE.search(line)
        if v:
            victim = int(v.group(1)); reason = (v.group(2) or "").strip()
    if pol and ways: yield pol, ways, victim, reason


# --- Exact-victim rules. The trace `dist` field shows the RAW circular distance
# (epoch+ne-curEpoch)%ne. Stamped-ness is now an EXPLICIT trace bit (a per-edge
# epoch was DELIVERED), NOT "epoch != 0" — a real epoch-0 line (low-ID next-ref)
# IS stamped. rrip_first/epoch_*/shortcircuit all treat an UNSTAMPED property line
# as effective distance 0 (stamped?dist:0).
def _eff_d(w):
    return w["dist"] if (w["prop"] == 1 and w["stamped"]) else 0


def _epoch_decisive(ways, victim, pol):
    """True iff the epoch DISTANCE strictly decided this victim — i.e. the policy reached the
    property/epoch branch (no record candidate vetoes it) AND the victim's effective epoch
    distance is a STRICT max among the COMPETING candidates the rule actually ranks. This is
    stronger than "a stamped property line was evicted": it rules out victims chosen because they
    were the only candidate or won an eff-dist tie by way/recency. Used to certify that
    stamped-epoch eviction was genuinely exercised, not just that property churned."""
    vw = ways[victim]
    if not (vw["prop"] == 1 and vw["stamped"]):
        return False
    if pol == "ECG:rrip_first":
        mx = max(w["rrpv"] for w in ways)
        cand = [w for w in ways if w["rrpv"] == mx]
        if any(w["prop"] == 0 for w in cand):
            return False  # a record at max-rrpv is evicted first -> epoch did not decide
        # _rrip_rule ranks max EFFECTIVE distance over ALL max-rrpv property (unstamped -> 0),
        # so the victim competes against every other max-rrpv candidate (rd-decisive: correct).
        others = [w for w in cand if w["way"] != vw["way"]]
    elif pol in ("ECG:epoch_first", "ECG:epoch_only"):
        if any(w["prop"] == 0 for w in ways):
            return False  # records evicted first (oldest by recency)
        # _epoch_rule ranks farthest `dist` among STAMPED property ONLY, so the victim must
        # strictly beat another STAMPED competitor (unstamped property does not compete).
        others = [w for w in ways if w["way"] != vw["way"] and w["prop"] == 1 and w["stamped"]]
    elif pol in ("ECG:shortcircuit", "ECG:shortcircuit+epoch"):
        if any(w["prop"] == 0 for w in ways):
            return False  # a record is evicted first (first-by-way) -> epoch did not decide
        # _shortcircuit_rule ranks _eff_d over ALL property (unstamped -> 0), so the victim
        # competes against EVERY other property line (mirror rrip_first; no max-rrpv gate). A
        # stamped victim with dist>0 strictly beats unstamped eff=0 lines -> decisive.
        others = [w for w in ways if w["way"] != vw["way"] and w["prop"] == 1]
    elif pol == "ECG:degree_first":
        mx = max(w["rrpv"] for w in ways)
        cand = [w for w in ways if w["rrpv"] == mx]
        if any(w["prop"] == 0 for w in cand):
            return False
        coldest = max(w["dbg"] for w in cand)
        tier = [w for w in cand if w["dbg"] == coldest]
        others = [
            w for w in tier
            if w["way"] != vw["way"] and w["prop"] == 1 and w["stamped"]
        ]
    else:
        return False
    if not others:
        return False  # no competitor -> epoch ranking did not decide
    return _eff_d(vw) > max(_eff_d(w) for w in others)


def _first_by(ways, key):
    return min(ways, key=lambda way: (*key(way), way["way"]))["way"]


def _select_epoch(ways):
    recs = [w for w in ways if w["prop"] == 0]
    if recs:
        return _first_by(recs, lambda way: (way["last"],))
    stamped_lines = [w for w in ways if w["prop"] == 1 and w["stamped"]]
    if stamped_lines:
        return _first_by(stamped_lines, lambda way: (-way["dist"],))
    return _first_by(ways, lambda way: (way["last"],))


def _select_rrip(ways):
    mx = max(w["rrpv"] for w in ways)
    cand = [w for w in ways if w["rrpv"] == mx]
    recs = [w for w in cand if w["prop"] == 0]
    if recs:
        return _first_by(recs, lambda way: (way["last"],))
    return _first_by(cand, lambda way: (-_eff_d(way),))


def _select_rrip_no_epoch(ways):
    mx = max(w["rrpv"] for w in ways)
    candidates = [way for way in ways if way["rrpv"] == mx]
    records = [way for way in candidates if way["prop"] == 0]
    if records:
        return _first_by(records, lambda way: (way["last"],))
    return min(way["way"] for way in candidates)


def _select_rrip_no_epoch_recency(ways):
    mx = max(w["rrpv"] for w in ways)
    candidates = [way for way in ways if way["rrpv"] == mx]
    records = [way for way in candidates if way["prop"] == 0]
    return _first_by(
        records if records else candidates,
        lambda way: (way["last"],))


def _select_degree(ways):
    mx = max(w["rrpv"] for w in ways)
    cand = [w for w in ways if w["rrpv"] == mx]
    recs = [w for w in cand if w["prop"] == 0]
    if recs:
        return _first_by(recs, lambda way: (way["last"],))
    return _first_by(
        cand,
        lambda way: (-way["dbg"], -_eff_d(way), way["last"]),
    )


def _select_future_tier(ways):
    mx = max(w["rrpv"] for w in ways)
    candidates = [way for way in ways if way["rrpv"] == mx]
    records = [way for way in candidates if way["prop"] == 0]
    if records:
        return _first_by(records, lambda way: (way["last"],))
    return _first_by(
        candidates,
        lambda way: (-_eff_d(way), -way["dbg"], way["last"]),
    )


def _select_shortcircuit(ways):
    recs = [w for w in ways if w["prop"] == 0]
    if recs:
        return min(w["way"] for w in recs)
    return _first_by(ways, lambda way: (-_eff_d(way), -way["dbg"]))


def _select_record_lru(ways):
    records = [way for way in ways if way["prop"] == 0]
    return _first_by(
        records if records else ways,
        lambda way: (way["last"],))


SELECTORS = {
    "LRU": lambda ways: _first_by(ways, lambda way: (way["last"],)),
    "GRASP": lambda ways: _first_by(
        ways, lambda way: (-way["rrpv"],)),
    "ECG:grasp_only": lambda ways: _first_by(
        ways, lambda way: (-way["rrpv"],)),
    "ECG:lru_only": lambda ways: _first_by(
        ways, lambda way: (way["last"],)),
    "ECG:record_lru": _select_record_lru,
    "ECG:shortcircuit": _select_shortcircuit,
    "ECG:shortcircuit+epoch": _select_shortcircuit,
    "ECG:epoch_first": _select_epoch,
    "ECG:epoch_only": _select_epoch,
    "ECG:rrip_first": _select_rrip,
    "ECG:rrip_no_epoch": _select_rrip_no_epoch,
    "ECG:rrip_no_epoch_recency": _select_rrip_no_epoch_recency,
    "ECG:degree_first": _select_degree,
    "ECG:future_tier_first": _select_future_tier,
}


def verify_trace(
        name, result, prefix="", reasons=None, coverage=None,
        expected_policy=None):
    """Assert each victim in a (text, ran_ok) result obeys its policy rule.
    Hard-fails on runner failure (no/empty trace) and on any emitted policy with
    no rule. Tallies eviction `reason=` strings into `reasons` for coverage.
    If `coverage` (a dict) is passed, also counts total victims and EPOCH-RANKED
    property victims (victim prop=1 stamped=1) so callers can detect a trace that
    only exercised record/recency decisions (epoch_victims==0 => the stamped-epoch
    eviction path was never exercised; a 'PASS' there is decision-only, not
    delivery-validated)."""
    text, ran_ok = result
    if not ran_ok:
        print(f"  {prefix}{name:14s}: runner FAILED (crash / no log)   [FAIL]")
        return False
    checked = passed = 0
    ok = True
    unknown = set()
    unexpected = set()
    stamp_violations = 0
    for pol, ways, victim, reason in parse_blocks(text):
        if reasons is not None and reason:
            reasons.add(reason)
        # Stamping-correctness invariant (C): a record (non-property) line must NEVER
        # carry a stamp — stamped=1 implies prop=1. The shared policy's effDist and the
        # rules below all assume this; a backend adapter that stamped a record (or a
        # kernel that failed to clear before a sequential read) would corrupt eviction.
        for w in ways:
            if w["prop"] == 0 and w["stamped"] == 1:
                stamp_violations += 1
                if stamp_violations <= 3:
                    print(f"  [STAMP-INVARIANT] {prefix}{name}/{pol}: way{w['way']} is a "
                          f"record (prop=0) but stamped=1 (records must never be stamped)")
        selector = SELECTORS.get(pol)
        if selector is None:
            unknown.add(pol); continue
        if expected_policy is not None and pol != expected_policy:
            unexpected.add(pol)
        if victim is None:
            continue
        checked += 1
        if coverage is not None:
            coverage["victims"] = coverage.get("victims", 0) + 1
            vwc = ways[victim]
            if vwc["prop"] == 1 and vwc["stamped"]:
                coverage["epoch_victims"] = coverage.get("epoch_victims", 0) + 1
                # tie-vs-collapse diagnostic: a stamped victim with dist>0 means a real (non-zero)
                # epoch was delivered (just tied with others -> do-no-harm); if ALL stamped victims
                # have dist==0 the delivered epochs COLLAPSED to 0 (a delivery-quality regression,
                # not do-no-harm). rd-decisive suggestion.
                if vwc["dist"] > 0:
                    coverage["epoch_victims_nz"] = coverage.get("epoch_victims_nz", 0) + 1
            if _epoch_decisive(ways, victim, pol):
                coverage["epoch_decisive"] = coverage.get("epoch_decisive", 0) + 1
        expected_victim = selector(ways)
        if victim == expected_victim:
            passed += 1
        else:
            ok = False
            print(f"  [VIOLATION] {prefix}{name}/{pol}: victim=way{victim} "
                  f"expected=way{expected_victim} "
                  f"ways={[ (w['way'],w['rrpv'],w['dist'],w['prop'],w['last']) for w in ways]}")
    if stamp_violations:  # records must never be stamped -> fail loudly
        ok = False
        print(f"  [STAMP-INVARIANT] {prefix}{name}: {stamp_violations} record(s) stamped (must be 0)")
    if unknown:  # an emitted policy with no checker is a coverage hole -> fail loudly
        ok = False
        print(f"  [UNKNOWN POL] {prefix}{name}: {sorted(unknown)} has no RULES entry")
    if unexpected:
        ok = False
        print(
            f"  [POLICY MISMATCH] {prefix}{name}: expected "
            f"{expected_policy}, emitted {sorted(unexpected)}")
    status = "OK " if ok and checked > 0 else ("NO-TRACE" if checked == 0 else "FAIL")
    print(f"  {prefix}{name:14s}: {passed}/{checked} evictions obey spec   [{status}]")
    return ok and checked > 0


SYNTH_BIN = ROOT / "bench" / "bin_sim" / "test_ecg_victim"
PARITY_SRC = ROOT / "bench" / "src_sim" / "test_ecg_packed_field_parity.cc"
PAIR_SRC = ROOT / "bench" / "src_sim" / "test_ecg_reuse_plan.cc"


def run_field_parity():
    """Compile + run the ISA field-layout parity test. Pins the shared
    ecg_mode6_builder.h pack/extract layout (cache_sim, gem5 kernel+decoder, Sniper all
    use it) AND the ISA DRIFT GUARD: the gem5 decoder_ecg_extract.isa hand-codes the wide
    shifts (dest>>0/epoch>>24/pfx>>40) instead of calling the builder — this asserts those
    hand-coded shifts still equal the shared builder, so a wide-layout change that forgets the
    .isa fails HERE (fast) instead of silently mis-decoding in gem5."""
    binp = Path("/tmp") / "verify_field_parity"
    cc = subprocess.run(["g++", "-O2", "-std=c++17", f"-I{ROOT}/bench/include",
                         f"-I{ROOT}/bench/include/cache_sim", str(PARITY_SRC), "-o", str(binp)],
                        capture_output=True, text=True)
    if cc.returncode != 0:
        print(f"  [field-parity] FAIL: compile error\n{cc.stderr[:400]}"); return False
    p = subprocess.run([str(binp)], capture_output=True, text=True, timeout=60)
    for line in p.stdout.splitlines():
        if "ISA drift" in line or "RESULT:" in line:
            print("  " + line.strip())
    ok = (p.returncode == 0)
    print(f"  field-parity (shared ISA layout + drift guard): [{'OK ' if ok else 'FAIL'}]")
    return ok


def run_reuse_plan_unit():
    """Build and run the shared pull/push ReusePlan builder + wire/distance test."""
    binp = Path("/tmp") / "verify_ecg_reuse_plan"
    cc = subprocess.run(
        ["g++", "-O2", "-std=c++17", f"-I{ROOT}/bench/include",
         str(PAIR_SRC), "-o", str(binp)],
        capture_output=True, text=True)
    if cc.returncode != 0:
        print(f"  [ReusePlan] FAIL: compile error\n{cc.stderr[:400]}")
        return False
    p = subprocess.run([str(binp)], capture_output=True, text=True, timeout=60)
    if p.stdout.strip():
        print("  " + p.stdout.strip())
    ok = p.returncode == 0
    print(f"  shared ReusePlan builder/wire/distance: [{'OK ' if ok else 'FAIL'}]")
    return ok


def verify_reuse_plan_trace(
        name, result, ne, prefix="", coverage=None,
        expected_policy=None, require_exact_bind=False,
        require_request_bound=False):
    """Verify two-epoch ReusePlan reached resident lines and each traced `dist` is
    min(distance(epoch1), distance(epoch2)). Combined with verify_trace's exact
    victim rule, this certifies the ReusePlan adapter and eviction decision."""
    text, ran_ok = result
    ok = verify_trace(
        name, result, prefix=prefix, coverage=coverage,
        expected_policy=expected_policy)
    if not ran_ok:
        return False
    current = None
    pairs = distinct = bad = 0
    expected = {}
    received = {}
    expected_widths = {}
    received_widths = {}
    duplicate_delivery_sequences = set()
    accepted = {}
    duplicate_accept_sequences = set()
    requests = {}
    duplicate_request_sequences = set()
    sideband = {}
    fused_receipts = []
    bound_consumes = {}
    duplicate_bound_sequences = set()
    duplicate_fused_sequences = set()
    fused_validation = None
    receipt_bind_match = False
    accepted_ok = False
    for line in text.splitlines():
        accepted_match = REUSE_PLAN_ACCEPT_RE.search(line)
        if accepted_match:
            groups = accepted_match.groups()
            trace_sequence = int(groups[0])
            if trace_sequence in accepted:
                duplicate_accept_sequences.add(trace_sequence)
            accepted[trace_sequence] = (
                int(groups[1]), int(groups[2]), int(groups[3]), groups[4],
                int(groups[5]), int(groups[6]), int(groups[7]),
                int(groups[8]), int(groups[9]), int(groups[10]),
            )
            continue
        request_match = REUSE_PLAN_REQUEST_RE.search(line)
        if request_match:
            groups = request_match.groups()
            trace_sequence = int(groups[0])
            if trace_sequence in requests:
                duplicate_request_sequences.add(trace_sequence)
            requests[trace_sequence] = tuple(map(int, groups[1:]))
            continue
        bound_match = REUSE_PLAN_BIND_CONSUME_RE.search(line)
        if bound_match:
            groups = bound_match.groups()
            sequence = int(groups[0])
            if sequence in bound_consumes:
                duplicate_bound_sequences.add(sequence)
            bound_consumes[sequence] = (
                int(groups[1]), int(groups[2], 16), int(groups[3], 16),
                int(groups[4]), int(groups[5]), int(groups[6]),
            )
            continue
        validated = REUSE_PLAN_FUSED_VALID_RE.search(line)
        if validated:
            fused_validation = tuple(map(int, validated.groups()))
            continue
        fused = REUSE_PLAN_FUSED_RECV_RE.search(line)
        if fused:
            groups = fused.groups()
            receipt = (
                int(groups[0]), int(groups[1]), int(groups[2]),
                int(groups[3], 16), *map(int, groups[4:]),
            )
            if receipt[0] in {item[0] for item in fused_receipts}:
                duplicate_fused_sequences.add(receipt[0])
            fused_receipts.append(receipt)
            continue
        delivery = REUSE_PLAN_DELIVERY_RE.search(line)
        if delivery:
            kind, _sim, seq, dest, tier, first, second, width = (
                delivery.groups())
            sequence = int(seq)
            target = (
                expected if kind == "EXPECT"
                else sideband if kind == "SIDEBAND"
                else received
            )
            if sequence in target:
                duplicate_delivery_sequences.add((kind, sequence))
            target[sequence] = (
                int(dest), int(tier), int(first), int(second))
            if kind == "EXPECT":
                expected_widths[sequence] = int(width or 4)
            elif kind == "RECV":
                received_widths[sequence] = int(width or 4)
            continue
        h = REUSE_PLAN_HDR_RE.search(line)
        if h:
            current = int(h.group(1))
            continue
        m = WAY_RE.search(line)
        if not m or current is None:
            continue
        groups = m.groups()
        epoch = int(groups[3])
        distance = int(groups[4])
        prop = int(groups[5])
        stamped = int(groups[6])
        epoch2 = int(groups[9]) if groups[9] is not None else epoch
        sched_n = int(groups[10]) if groups[10] is not None else 1
        if sched_n < 2 or not (prop and stamped):
            continue
        pairs += 1
        distinct += epoch != epoch2
        d1 = (min(epoch, ne - 1) + ne - (current % ne)) % ne
        d2 = (min(epoch2, ne - 1) + ne - (current % ne)) % ne
        bad += distance != min(d1, d2)
    pair_live = pairs > 0 and distinct > 0
    requires_delivery_trace = not name.startswith("cache_sim/")
    delivery_ok = not requires_delivery_trace
    if sideband and fused_receipts:
        required = set(range(32))
        fused_valid = all(
            begin <= index < end and vpl > 0 and dest // vpl == line_id and
            1 <= tier <= 3
            for (_seq, _src, line_id, _addr_line, vpl, index, begin, end,
                 dest, tier, _first, _second) in fused_receipts
        )
        receipt_by_seq = {
            receipt[0]: receipt for receipt in fused_receipts
        }
        bind_valid = (
            set(bound_consumes) == required and
            all(
                size > 0 and (size & (size - 1)) == 0 and
                (bound & ~(size - 1)) == line and
                0 <= current < ne and context > 0
                for (_core, bound, line, size, current, context)
                in bound_consumes.values()
            )
        )
        receipt_bind_match = (
            set(receipt_by_seq) == required and bind_valid and
            not duplicate_bound_sequences and
            not duplicate_fused_sequences and
            len(fused_receipts) == len(receipt_by_seq) and
            all(
                receipt_by_seq[sequence][3] ==
                bound_consumes[sequence][2]
                for sequence in required
            )
        )
        sideband_tier_valid = all(
            0 <= fields[1] <= 3 for fields in sideband.values()
        ) and any(fields[1] > 0 for fields in sideband.values())
        expected_tier_valid = all(
            0 <= fields[1] <= 3 for fields in expected.values()
        )
        # Fused Sniper delivery is property-line coalesced: multiple raw edge
        # records can target one line, so request order is not expected to match
        # the raw sideband sequence. validate_sniper_fused_receipts independently
        # checks every receipt's raw index and exact packed record against the
        # exported ReusePlan files; this verifier additionally pins line/dest/tier shape.
        exact_bind_ok = receipt_bind_match if require_exact_bind else True
        delivery_ok = (
            set(expected) == required and
            set(sideband) == required and sideband_tier_valid and
            expected_tier_valid and fused_valid and
            not duplicate_delivery_sequences and
            exact_bind_ok and
            fused_validation is not None and
            fused_validation[0] > 0 and fused_validation[1] == 0
        )
    elif sideband:
        required = set(range(32))
        fused_valid = all(
            begin <= index < end and vpl > 0 and dest // vpl == line_id and
            1 <= tier <= 3
            for (_seq, _src, line_id, _addr_line, vpl, index, begin, end,
                 dest, tier, _first, _second) in fused_receipts
        )
        tier_valid = all(
            0 <= fields[1] <= 3 for fields in expected.values()
        ) and all(
            0 <= fields[1] <= 3 for fields in sideband.values()
        ) and any(fields[1] > 0 for fields in sideband.values())
        delivery_ok = (
            set(expected) == required and
            set(sideband) == required and
            expected == sideband and
            tier_valid and
            not duplicate_delivery_sequences and
            bool(fused_receipts) and fused_valid and
            fused_validation is not None and
            fused_validation[0] > 0 and fused_validation[1] == 0
        )
    elif requires_delivery_trace or expected or received:
        required = set(range(32))
        tier_valid = all(
            0 <= fields[1] <= 3 for fields in expected.values()
        ) and all(
            0 <= fields[1] <= 3 for fields in received.values()
        ) and any(fields[1] > 0 for fields in received.values())
        if requires_delivery_trace:
            accepted_common_ok = (
                set(expected) == required and
                set(expected_widths) == required and
                bool(accepted) and
                set(accepted).issubset(required) and
                not duplicate_accept_sequences and
                len({item[0] for item in accepted.values()}) ==
                len(accepted) and
                all(
                    (request_dest * width) // 64 ==
                    (fill_dest * width) // 64 and
                    1 <= tier <= 3 and
                    0 <= first < ne and
                    0 <= second < ne and
                    width in (4, 8) and
                    0 <= current < ne and
                    context > 0 and
                    source in ("request", "mailbox")
                    for (
                        _request_sequence, request_dest, fill_dest, source,
                        tier, first, second, current, context, width,
                    ) in accepted.values()
                )
            )
            accept_sources = {item[3] for item in accepted.values()}
            mailbox_accept_ok = (
                accept_sources == {"mailbox"} and
                not requests and
                all(
                    request_sequence == sequence and
                    request_dest == expected[sequence][0] and
                    tier == expected[sequence][1] and
                    first == expected[sequence][2] and
                    second == expected[sequence][3] and
                    width == expected_widths[sequence]
                    for sequence, (
                        request_sequence, request_dest, _fill_dest, _source,
                        tier, first, second, _current, _context, width,
                    ) in accepted.items()
                )
            )
            request_records_ok = (
                set(requests) == required and
                not duplicate_request_sequences and
                all(
                    requests[sequence][1] == expected[sequence][0] and
                    requests[sequence][2] == expected[sequence][1] and
                    requests[sequence][3] == expected[sequence][2] and
                    requests[sequence][4] == expected[sequence][3] and
                    0 <= requests[sequence][5] < ne and
                    requests[sequence][6] > 0
                    for sequence in required
                )
            )
            request_accept_ok = (
                accept_sources == {"request"} and
                request_records_ok and
                all(
                    sequence in requests and
                    request_sequence == requests[sequence][0] and
                    request_dest == requests[sequence][1] and
                    request_dest == expected[sequence][0] and
                    tier == requests[sequence][2] ==
                    expected[sequence][1] and
                    first == requests[sequence][3] ==
                    expected[sequence][2] and
                    second == requests[sequence][4] ==
                    expected[sequence][3] and
                    current == requests[sequence][5] and
                    context == requests[sequence][6]
                    for sequence, (
                        request_sequence, request_dest, _fill_dest, _source,
                        tier, first, second, current, context, _width,
                    ) in accepted.items()
                )
            )
            accepted_ok = (
                accepted_common_ok and
                (request_accept_ok if require_request_bound
                 else request_accept_ok or mailbox_accept_ok)
            )
        else:
            accepted_ok = True
        delivery_ok = (
            set(expected) == required and
            set(received) == required and
            expected == received and
            expected_widths == received_widths and
            tier_valid and not duplicate_delivery_sequences and accepted_ok
        )
    live = pair_live or (delivery_ok and len(expected) == 32)
    if coverage is not None:
        coverage["reuse_plan_ways"] = pairs
        coverage["reuse_plan_distinct_ways"] = distinct
        coverage["reuse_plan_distance_mismatches"] = bad
        coverage["reuse_plan_delivery_records"] = len(expected)
        coverage["reuse_plan_delivery_match"] = delivery_ok
        coverage["reuse_plan_delivery_widths"] = sorted(set(expected_widths.values()))
        coverage["reuse_plan_received_widths"] = sorted(set(received_widths.values()))
        coverage["reuse_plan_delivery_width_match"] = (
            expected_widths == received_widths)
        coverage["reuse_plan_accept_widths"] = sorted({
            item[9] for item in accepted.values()
        })
        coverage["reuse_plan_accept_sources"] = sorted({
            item[3] for item in accepted.values()
        })
        coverage["reuse_plan_accept_request_sequences"] = sorted({
            item[0] for item in accepted.values()
        })
        coverage["reuse_bind_request_records"] = len(requests)
        coverage["reuse_bind_request_bound_required"] = require_request_bound
        coverage["reuse_plan_accept_valid"] = accepted_ok
        coverage["reuse_plan_bind_consumes"] = len(bound_consumes)
        coverage["reuse_plan_bind_consume_valid"] = (
            receipt_bind_match if require_exact_bind else
            bool(bound_consumes) and all(
                size > 0 and (size & (size - 1)) == 0 and
                (bound & ~(size - 1)) == line and
                0 <= current < ne and context > 0
                for (_core, bound, line, size, current, context)
                in bound_consumes.values()))
        coverage["reuse_plan_bind_receipt_match"] = receipt_bind_match
        coverage["reuse_plan_exact_bind_required"] = require_exact_bind
        coverage["reuse_plan_fused_receipts"] = len(fused_receipts)
        coverage["reuse_plan_fused_vertices_per_line"] = sorted({
            receipt[4] for receipt in fused_receipts
        })
    if bad or not live or not delivery_ok:
        ok = False
    print(f"  {prefix}{name:14s}: ReusePlan ways={pairs} distinct={distinct} "
          f"distance_mismatches={bad} delivery={len(expected)}/"
          f"{len(sideband) if sideband else len(received)}"
          f"{' match' if delivery_ok else ' MISMATCH'}   "
          f"[{'OK ' if ok and live else 'FAIL'}]")
    return ok and live and delivery_ok


def verify_reuse_bind_request_accepts(
        name, text, expected_width=4, expected_line_bytes=64,
        coverage=None):
    """Verify O3 request binding without assuming program-order execution.

    O3 may execute and replay custom loads out of order, so guest-side EXPECT
    sequence numbers are not a stable key. The Request sequence is: every LLC
    accept must point at an emitted request and reproduce its destination and
    complete ReusePlan payload on the exact filled line.
    """
    requests = {}
    accepts = []
    duplicate_requests = set()
    for line in text.splitlines():
        request_match = REUSE_PLAN_REQUEST_RE.search(line)
        if request_match:
            groups = request_match.groups()
            request_sequence = int(groups[1])
            if request_sequence in requests:
                duplicate_requests.add(request_sequence)
            requests[request_sequence] = tuple(map(int, groups[2:]))
            continue
        accept_match = REUSE_PLAN_ACCEPT_RE.search(line)
        if accept_match:
            groups = accept_match.groups()
            accepts.append((
                int(groups[1]), int(groups[2]), int(groups[3]), groups[4],
                int(groups[5]), int(groups[6]), int(groups[7]),
                int(groups[8]), int(groups[9]), int(groups[10]),
            ))

    bad = 0
    for (
            request_sequence, request_dest, fill_dest, source,
            tier, epoch1, epoch2, current, context, width) in accepts:
        requested = requests.get(request_sequence)
        observed = (
            request_dest, tier, epoch1, epoch2, current, context)
        same_line = (
            expected_width > 0 and expected_line_bytes > 0 and
            (request_dest * expected_width) // expected_line_bytes ==
            (fill_dest * expected_width) // expected_line_bytes)
        if (
                source != "request" or width != expected_width or
                not same_line or requested != observed):
            bad += 1

    ok = (
        bool(requests) and bool(accepts) and
        not duplicate_requests and bad == 0)
    if coverage is not None:
        coverage.update({
            "reuse_plan_o3_requests": len(requests),
            "reuse_plan_o3_accepts": len(accepts),
            "reuse_plan_o3_request_accept_mismatches": bad,
            "reuse_plan_o3_request_width": expected_width,
            "reuse_plan_o3_request_line_bytes": expected_line_bytes,
        })
    print(
        f"  {name}: O3 requests={len(requests)} accepts={len(accepts)} "
        f"request/fill/payload mismatches={bad} "
        f"{'[PASS]' if ok else '[FAIL]'}")
    return ok


def verify_unknown_mode_hardfails():
    """Negative test: an unrecognized ECG_MODE must HARD-FAIL (exit!=0 + [FATAL]),
    not silently fall back to DBG_PRIMARY. Silent fallback would run a different
    policy than requested while labelling itself as the requested mode. This is the
    safety gate that must hold before any ECG mode can be deleted/renamed."""
    env = {**os.environ, **BASE_ENV, "CACHE_POLICY": "ECG", "ECG_MODE": "BOGUS_MODE_XYZ"}
    p = subprocess.run([str(PR), *GRAPH_ARGS, "-o", "0", "-n", "1", "-i", "1"],
                       env=env, capture_output=True, text=True, timeout=120)
    hard_failed = (p.returncode != 0) and ("[FATAL]" in p.stderr) and ("BOGUS_MODE_XYZ" in p.stderr)
    print(f"  unknown ECG_MODE hard-fails (exit={p.returncode}, [FATAL] emitted): "
          f"{'[OK ]' if hard_failed else '[FAIL]'}")
    return hard_failed


def verify_unknown_variant_hardfails():
    """Negative test: an unrecognized ECG_VARIANT must abort, not fall back."""
    subprocess.run(
        ["make", "bench/bin_sim/test_ecg_victim"], cwd=str(ROOT),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    if not SYNTH_BIN.exists():
        print("  unknown ECG_VARIANT hard-fail: [FAIL] binary missing")
        return False
    env = {**os.environ, "ECG_VARIANT": "BOGUS_VARIANT_XYZ"}
    process = subprocess.run(
        [str(SYNTH_BIN)], env=env, capture_output=True, text=True, timeout=60)
    hard_failed = (
        process.returncode != 0 and
        "[FATAL] unknown ECG_VARIANT=BOGUS_VARIANT_XYZ" in process.stderr)
    print(
        "  unknown ECG_VARIANT hard-fails: "
        f"{'[OK ]' if hard_failed else '[FAIL]'}")
    return hard_failed


def run_synthetic():
    """Build + run the synthetic deterministic victim test: controlled 8-way sets
    with hand-computed exact victims. This is the part that actually exercises the
    epoch-property ranking (the live PageRank trace only ever evicts records) and
    pins the EXACT victim (not just necessary conditions), independent of the
    simulator's self-reported state."""
    subprocess.run(["make", "bench/bin_sim/test_ecg_victim"], cwd=str(ROOT),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    if not SYNTH_BIN.exists():
        print("  [synthetic] FAIL: could not build test_ecg_victim"); return False
    ok = True
    for variant in ["tier", "dueling", "admission_dueling",
                    "grasp_only", "epoch_only", "rrip_first",
                    "epoch_first", "degree_first", "lru_only", "record_lru",
                    "rrip_no_epoch", "rrip_no_epoch_recency",
                    "future_tier_first",
                    "shortcircuit"]:
        p = subprocess.run([str(SYNTH_BIN)], env={**os.environ, "ECG_VARIANT": variant},
                           capture_output=True, text=True, timeout=60)
        for line in p.stdout.splitlines():
            if "expect=" in line or line.startswith("[test_ecg_victim]") or "RESULT[tier]" in line:
                print("  " + line.rstrip())
        if p.returncode != 0:
            ok = False
    return ok


def _epoch_decided(pol, ways, v):
    """True iff the epoch VALUE selected this victim: the victim is a stamped
    property line chosen as the farthest among >=2 stamped property lines with
    distinct epochs, within the candidate pool that variant ranks. (shortcircuit
    ranks property by RAW dist, so unstamped property—huge raw dist—is evicted
    first and the stamped-epoch comparison is rarely operative; that variant's
    epoch ranking is covered by the synthetic test instead.)"""
    if v is None or ways[v]["prop"] != 1 or not ways[v]["stamped"]:
        return False
    if pol in ("ECG:rrip_first", "ECG:future_tier_first"):
        mx = max(w["rrpv"] for w in ways)
        pool = [w for w in ways if w["rrpv"] == mx and w["prop"] == 1 and w["stamped"]]
    elif pol in ("ECG:epoch_first", "ECG:epoch_only"):
        pool = [w for w in ways if w["prop"] == 1 and w["stamped"]]
    else:
        return False
    return (len(pool) >= 2 and len({w["dist"] for w in pool}) >= 2
            and ways[v]["dist"] == max(w["dist"] for w in pool))


def _count_epoch_decided(text):
    return sum(1 for pol, ways, v, r in parse_blocks(text) if _epoch_decided(pol, ways, v))


def verify_epoch_coverage(name, result, prefix="", strict=True):
    """Like verify_trace, but ALSO consider whether the epoch VALUE genuinely
    selected the victim (a stamped property line chosen as farthest among >=2
    distinct-epoch competitors). With strict=True, fail if that never happened
    (so the check cannot pass vacuously). With strict=False, report the count but
    do not fail on it — used where the model cannot keep a live L3 epoch stamp
    (gem5 has no ECG_STORED_REFRESH, so property reaches the L3 unstamped; its
    epoch ranking is covered by the cache_sim mirror + the synthetic test)."""
    text, ran_ok = result
    ok = verify_trace(name, result, prefix=prefix)
    if not ran_ok:
        return False
    comp = _count_epoch_decided(text)
    tag = f"{prefix}{name}"
    if comp == 0:
        verdict = "FAIL" if strict else "info: covered by cache_sim mirror"
        print(f"  [COVERAGE] {tag}: epoch value never selected the victim  [{verdict}]")
        return False if strict else ok
    print(f"  [COVERAGE] {tag}: {comp} evictions where the epoch value selected the victim  [OK]")
    return ok


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description="Assert each L3 policy obeys its spec.")
    ap.add_argument("--gem5", action="store_true",
                    help="Also verify the gem5 ECG_GRASP_POPT variants (slower; needs gem5.opt).")
    ap.add_argument("--sniper", action="store_true",
                    help="Also verify Sniper ECG variants (guarded sg_kernel run under prlimit).")
    ap.add_argument(
        "--gem5-isa-only", action="store_true",
        help="Run only the real RISC-V decoder plus O3 ReuseBind proposal probe.")
    ap.add_argument(
        "--isa-receipt-dir", default="",
        help="Persist real-decoder commands, logs, hashes, and result JSON.")
    args = ap.parse_args(argv)

    if args.gem5_isa_only:
        if not GEM5_OPT.exists():
            print(f"FAIL: build gem5 first: {GEM5_OPT}")
            return 2
        return 0 if run_gem5_isa_modes(
            Path(args.isa_receipt_dir)
            if args.isa_receipt_dir else None) else 1

    if not PR.exists():
        print(f"FAIL: build cache_sim first (make sim-pr): {PR}"); return 2
    suites = [("LRU", dict(CACHE_POLICY="LRU", CACHE_L3_POLICY="LRU")),
              ("GRASP", dict(CACHE_POLICY="GRASP", CACHE_L3_POLICY="GRASP")),
              ("grasp_only", {**ECG_ENV, "ECG_VARIANT": "grasp_only"}),
              ("epoch_only", {**ECG_ENV, "ECG_VARIANT": "epoch_only"}),
              ("rrip_first", {**ECG_ENV, "ECG_VARIANT": "rrip_first"}),
              ("rrip_no_epoch", {
                  **ECG_ENV, "ECG_VARIANT": "rrip_no_epoch"}),
              ("rrip_no_epoch_recency", {
                  **ECG_ENV, "ECG_VARIANT": "rrip_no_epoch_recency"}),
              ("epoch_first", {**ECG_ENV, "ECG_VARIANT": "epoch_first"}),
              ("degree_first", {**ECG_ENV, "ECG_VARIANT": "degree_first"}),
              ("future_tier_first", {
                  **ECG_ENV, "ECG_VARIANT": "future_tier_first"}),
              ("shortcircuit", {**ECG_ENV, "ECG_VARIANT": "shortcircuit"})]
    ok_all = True
    live_reasons = set()
    print("== synthetic deterministic victim tests (EXACT victim; exercises the epoch branch) ==")
    ok_all &= run_synthetic()
    print("\n-- ISA field-layout parity + drift guard (shared ecg_mode6_builder.h) --")
    ok_all &= run_field_parity()
    print("\n-- two-epoch ReusePlan shared builder + wire/distance parity --")
    ok_all &= run_reuse_plan_unit()
    print("\n-- negative test: unknown ECG_MODE must hard-fail (not silent DBG_PRIMARY) --")
    ok_all &= verify_unknown_mode_hardfails()
    print(f"\n-- cache_sim (L3 policies, {GRAPH_LABEL}; live-trace integration) --")
    for name, env in suites:
        ok_all &= verify_trace(name, run(env), reasons=live_reasons)

    # Epoch-coverage: force the epoch-property branch live (big L2 + small L3 +
    # ECG_STORED_REFRESH) and assert the tightened exact rules hold AND the epoch
    # value genuinely broke property ties. This exercises ECG's core eviction on
    # the REAL simulator end-to-end (not just the synthetic unit test).
    print("\n-- cache_sim epoch-coverage (forced property eviction; tightened exact rules) --")
    for variant in [
            "rrip_first", "future_tier_first",
            "epoch_first", "epoch_only"]:
        ok_all &= verify_epoch_coverage(variant, run({**ECG_ENV, "ECG_VARIANT": variant}, COV_ENV))
    # shortcircuit ranks property by RAW dist (evicts unstamped first), so its
    # stamped-epoch ranking is rarely operative live; verify its exact rule here
    # and rely on the synthetic test for its stamped-epoch + DBG-tiebreak path.
    ok_all &= verify_trace("shortcircuit", run({**ECG_ENV, "ECG_VARIANT": "shortcircuit"}, COV_ENV),
                           prefix="(sc) ")

    # Cross-kernel coverage (B+C): BC's bottom-up traversal NATURALLY evicts PROPERTY
    # lines (the PR workload only ever evicts records), so this is the only LIVE check
    # of the epoch-eviction branch on a real kernel via a DIFFERENT adapter access
    # pattern. verify_trace also enforces the record-never-stamped invariant (C).
    if BC.exists():
        print("\n-- cache_sim BC cross-kernel (BC evicts property -> live epoch branch + stamp invariant) --")
        for variant in ["grasp_only", "epoch_only", "rrip_first",
                        "rrip_no_epoch", "rrip_no_epoch_recency",
                        "epoch_first", "degree_first",
                        "future_tier_first",
                        "shortcircuit"]:
            ok_all &= verify_trace(f"bc/{variant}", run_bc({**ECG_ENV, "ECG_VARIANT": variant}, COV_ENV),
                                   prefix="(bc) ", reasons=live_reasons)
        # BC epoch-coverage is INFORMATIONAL (strict=False): BC's property lines carry
        # uniform/fallback epochs under this geometry (it is not the full per-edge-mask
        # delivery kernel that PR is), so the epoch VALUE rarely discriminates between
        # >=2 candidates. The strict epoch-discrimination is covered by PR's epoch-
        # coverage + the synthetic test; BC's value here is the LIVE property-eviction
        # spec-compliance (above) on a different adapter access pattern.
        for variant in ["rrip_first", "epoch_first", "epoch_only"]:
            ok_all &= verify_epoch_coverage(f"bc/{variant}",
                                            run_bc({**ECG_ENV, "ECG_VARIANT": variant}, COV_ENV),
                                            prefix="(bc) ", strict=False)
    else:
        print("  [skip] BC binary not built (make sim-bc) — skipping cross-kernel coverage")

    if args.gem5:
        if not GEM5_OPT.exists():
            print(f"FAIL: build gem5 first: {GEM5_OPT}"); return 2
        # Record the one gem5.opt used for every configuration in this run.
        # uses for ALL variant x cache combinations below. ECG_VARIANT is read at runtime via
        # getenv (ecg_rp.cc) and the cache geometry is a gem5 CLI arg, so the 5 tie-breaks x
        # 2 L3 sizes (16kB variants + 4kB coverage) below ALL run on this single binary — i.e.
        # tie-breaks and caches are swept at RUNTIME, no recompile. Only the 64-bit record
        # LAYOUT is compile-time, and the field-parity drift guard above pins it.
        import hashlib
        gem5_md5 = hashlib.md5(GEM5_OPT.read_bytes()).hexdigest()[:12]
        print(f"\n-- gem5 compile-once: ONE binary gem5.opt md5={gem5_md5} drives all "
              f"tie-break x cache runs below (runtime sweep, no recompile) --")
        print("\n-- gem5 ISA decode: every ecg.load (mode x dest-width) through the REAL decoder + teeth --")
        ok_all &= run_gem5_isa_modes(
            Path(args.isa_receipt_dir)
            if args.isa_receipt_dir else None)
        print(f"\n-- gem5 (ECG_GRASP_POPT variants, {GRAPH_LABEL}/-o5) --")
        for variant in ["grasp_only", "epoch_only", "rrip_first",
                        "rrip_no_epoch", "rrip_no_epoch_recency",
                        "epoch_first", "degree_first",
                        "future_tier_first", "shortcircuit"]:
            ok_all &= verify_trace(variant, run_gem5(variant), prefix="gem5 ", reasons=live_reasons)
        print("\n-- gem5 epoch-coverage (exact rules on forced geometry; epoch-value gate informational) --")
        for variant in ["rrip_first", "epoch_first"]:
            ok_all &= verify_epoch_coverage(variant, run_gem5(variant, cov=True), prefix="gem5 ", strict=False)

    if args.sniper:
        # grasp_only delegates to the shared SRRIP path (no ECG trace); verify the
        # four ECG-specific variants. Runs are memory-capped (Sniper/SDE runaway).
        print(f"\n-- sniper (ECG_GRASP_POPT variants, {GRAPH_LABEL}/-o5, guarded) --")
        for variant in ["epoch_only", "rrip_first", "epoch_first",
                        "degree_first", "future_tier_first",
                        "shortcircuit"]:
            ok_all &= verify_trace(variant, run_sniper(variant), prefix="sniper ", reasons=live_reasons)

    # Live default-geometry coverage note: that workload only ever evicts records,
    # so the epoch branch is exercised by the synthetic + epoch-coverage runs above.
    epoch_reasons = {r for r in live_reasons if "epoch property" in r or "farthest" in r}
    print("\n-- live-trace branch coverage (default geometry) --")
    print(f"  live eviction reasons seen: {sorted(live_reasons) or '(none)'}")
    print(f"  epoch-property branch fired in default geom: {'yes' if epoch_reasons else 'NO (covered by synthetic + epoch-coverage runs)'}")

    # ---------------------------------------------------------------------- #
    # BEHAVIORAL equivalence + INSERTION-RRPV invariant. The per-sim eviction-
    # spec checks above are necessary but NOT sufficient: they pass even with a
    # backwards insertion RRPV (gem5 non-property=2 backfire bug), an unreordered
    # workload (Sniper sg_kernel -o ignored), or cross-sim direction disagreement.
    # These gates enforce behavioral correctness/equivalence. See verify/equiv.py.
    # ---------------------------------------------------------------------- #
    print("\n" + "=" * 72)
    sims = ["cache_sim"] + (["gem5"] if args.gem5 else []) + (["sniper"] if args.sniper else [])
    ok_all &= check_behavioral_equivalence(sims)
    ok_all &= check_insertion_rrpv_invariant()

    print("\nRESULT:", "ALL POLICIES VERIFIED ✓" if ok_all else "VERIFICATION FAILED ✗")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
