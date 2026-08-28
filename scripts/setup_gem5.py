#!/usr/bin/env python3
"""
Setup script for gem5 integration with GraphBrew.

Clones gem5 into bench/include/gem5_sim/gem5/, applies GraphBrew overlay patches
(GRASP, P-OPT, ECG replacement policies and DROPLET prefetcher), and builds
gem5 for the requested ISAs.

Usage:
    python scripts/setup_gem5.py                        # Clone + build X86 (default)
    python scripts/setup_gem5.py --isa X86 RISCV        # Build both ISAs
    python scripts/setup_gem5.py --isa RISCV --build-type debug
    python scripts/setup_gem5.py --skip-build            # Clone + patch only
    python scripts/setup_gem5.py --clean                 # Remove cloned gem5
    python scripts/setup_gem5.py --rebuild               # Force rebuild

The script is idempotent: re-running it will skip completed steps.
"""

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

# =============================================================================
# Constants
# =============================================================================

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
BENCH_DIR = PROJECT_ROOT / "bench"
GEM5_SIM_DIR = BENCH_DIR / "include" / "gem5_sim"
GEM5_DIR = GEM5_SIM_DIR / "gem5"
OVERLAYS_DIR = GEM5_SIM_DIR / "overlays"
VERSION_FILE = GEM5_SIM_DIR / ".gem5_version"
PATCH_STATE_FILE = GEM5_SIM_DIR / ".gem5_patch_state.json"

GEM5_REPO_URL = "https://github.com/gem5/gem5.git"
GEM5_DEFAULT_TAG = "v24.0"
GEM5_DEFAULT_COMMIT = "b1a44b89c7bae73fae2dc547bc1f871452075b85"

VALID_ISAS = ("X86", "RISCV", "ARM")
VALID_BUILD_TYPES = ("opt", "debug", "fast")

# Overlay mappings: source (relative to overlays/) -> destination (relative to gem5/src/)
OVERLAY_FILE_MAP = {
    "../../hawkeye_policy.h":
        "mem/cache/replacement_policies/hawkeye_policy.h",
    # Replacement policies
    "mem/cache/replacement_policies/hawkeye_rp.hh":
        "mem/cache/replacement_policies/hawkeye_rp.hh",
    "mem/cache/replacement_policies/hawkeye_rp.cc":
        "mem/cache/replacement_policies/hawkeye_rp.cc",
    "mem/cache/replacement_policies/grasp_rp.hh":
        "mem/cache/replacement_policies/grasp_rp.hh",
    "mem/cache/replacement_policies/grasp_rp.cc":
        "mem/cache/replacement_policies/grasp_rp.cc",
    "mem/cache/replacement_policies/popt_rp.hh":
        "mem/cache/replacement_policies/popt_rp.hh",
    "mem/cache/replacement_policies/popt_rp.cc":
        "mem/cache/replacement_policies/popt_rp.cc",
    "mem/cache/replacement_policies/ecg_rp.hh":
        "mem/cache/replacement_policies/ecg_rp.hh",
    "mem/cache/replacement_policies/ecg_rp.cc":
        "mem/cache/replacement_policies/ecg_rp.cc",
    "mem/cache/replacement_policies/ecg_victim_policy.hh":
        "mem/cache/replacement_policies/ecg_victim_policy.hh",
    "mem/cache/replacement_policies/ecg_mode.hh":
        "mem/cache/replacement_policies/ecg_mode.hh",
    "mem/cache/replacement_policies/ecg_reuse_bind_request_ext.hh":
        "mem/cache/replacement_policies/ecg_reuse_bind_request_ext.hh",
    "mem/cache/replacement_policies/graph_cache_context_gem5.hh":
        "mem/cache/replacement_policies/graph_cache_context_gem5.hh",
    "mem/cache/replacement_policies/GraphReplacementPolicies.py":
        "mem/cache/replacement_policies/GraphReplacementPolicies.py",
    # Prefetcher
    "mem/cache/prefetch/droplet.hh":
        "mem/cache/prefetch/droplet.hh",
    "mem/cache/prefetch/droplet.cc":
        "mem/cache/prefetch/droplet.cc",
    "mem/cache/prefetch/ecg_pfx.hh":
        "mem/cache/prefetch/ecg_pfx.hh",
    "mem/cache/prefetch/ecg_pfx.cc":
        "mem/cache/prefetch/ecg_pfx.cc",
    "mem/cache/prefetch/GraphPrefetchers.py":
        "mem/cache/prefetch/GraphPrefetchers.py",
    # RISC-V ECG custom instruction scaffold
    "arch/riscv/isa/formats/ecg.isa":
        "arch/riscv/isa/formats/ecg.isa",
}

# Patches to apply (relative to overlays/)
PATCH_FILES = [
    "mem/cache/replacement_policies/SConscript.patch",
    "mem/cache/prefetch/SConscript.patch",
]

# Unified-diff patches to apply via `patch -p1` (relative to overlays/).
# Each entry: (overlay_relpath, target_dir_relative_to_gem5_root)
# These are tracked as patches (not full file copies) so upstream gem5
# changes are easier to merge.
UNIFIED_DIFF_PATCHES = [
    # Architectural ReusePlan state: user-level custom CSRs 0x800/0x801 backed by
    # per-hart RISC-V MiscReg storage. This state is automatically copied and
    # checkpointed by gem5's ISA machinery.
    ("arch/riscv/ecg_csr.patch", "."),
    # Reserved VTYPE.vsew values are legal encodings that set vill. Guard the
    # O3 branch-target path so speculative decoding cannot abort gem5 before
    # the illegal vector configuration is squashed.
    ("arch/riscv/vector_vtype_guard.patch", "."),
    # Queue-servicing fix: nextPrefetchReadyTime returns curTick()
    # when pfqMissingTranslation has entries even if pfq is empty.
    # Required for prefetchers like ECG_PFX that emit only cross-page
    # candidates.
    ("mem/cache/prefetch/queued_hh.patch", "."),
    # Latency-readiness guard: getPacket() returns nullptr if the
    # front-of-queue prefetch's tick is in the future, preserving the
    # prefetcher's latency contract for cycle-accurate parity.
    ("mem/cache/prefetch/queued_cc_latency.patch", "."),
    # ECG masked-load OoO producer: bind the per-dynamic graph mask
    # ({dest,tier,epoch1,epoch2} for ReusePlan) to the property load's own demand
    # Request so it reaches the LLC without relying on a shared mailbox under
    # DerivO3CPU. exec_context.hh adds default-noop hint hooks;
    # o3/dyn_inst.hh overrides it with per-dynamic state; o3/lsq.cc attaches the
    # extension in LSQRequest::addReq (gated on env GEM5_ECG_PRODUCER). The
    # ea_code caller lives in the decoder_ecg_extract.isa snippet.
    ("cpu/exec_context_ecg_producer.patch", "."),
    ("cpu/o3/dyn_inst_ecg_producer.patch", "."),
    ("cpu/o3/lsq_ecg_producer.patch", "."),
    # Pass the allocating Request to replacement victim selection so ReusePlan uses
    # the request-carried current epoch/context rather than global magic state.
    ("mem/cache/ecg_victim_request.patch", "."),
    # Coalesced ReusePlan requests retain the latest same-hart/context sequence and
    # fail closed on cross-hart/context or ordinary-request conflicts.
    ("mem/cache/mshr_ecg_merge.patch", "."),
    # FlowThrough: packed ECG record misses retain normal L1/L2 fills but
    # suppress shared-L3 allocation through the MSHR allocOnFill bit.
    ("mem/request_flowthrough.patch", "."),
    ("mem/cache/base_flowthrough.patch", "."),
    ("mem/cache/base_flowthrough_request_flag.patch", "."),
    ("mem/cache/array_attribution.patch", "."),
    ("mem/cache/prefetch_flowthrough.patch", "."),
]


def unified_patch_target_paths() -> list[Path]:
    targets = set()
    for overlay_rel, _target_dir in UNIFIED_DIFF_PATCHES:
        patch_file = OVERLAYS_DIR / overlay_rel
        for line in patch_file.read_text().splitlines():
            if line.startswith("+++ b/"):
                targets.add(GEM5_DIR / line[len("+++ b/"):])
    targets.update({
        GEM5_DIR / "src/mem/cache/replacement_policies/SConscript",
        GEM5_DIR / "src/mem/cache/prefetch/SConscript",
        GEM5_DIR / "src/sim/pseudo_inst.cc",
        GEM5_DIR / "src/arch/riscv/isa/formats/formats.isa",
        GEM5_DIR / "src/arch/riscv/isa/includes.isa",
        GEM5_DIR / "src/arch/riscv/isa/decoder.isa",
    })
    return sorted(targets)


def installed_patch_target_hashes() -> dict[str, str]:
    hashes = {}
    for path in unified_patch_target_paths():
        if not path.exists():
            raise SystemExit(f"Required patched gem5 target missing: {path}")
        relative = str(path.relative_to(GEM5_DIR))
        hashes[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    return hashes


def verify_existing_patch_state() -> None:
    """Refuse to patch over a checkout that drifted since its last verification."""
    try:
        patch_state = json.loads(PATCH_STATE_FILE.read_text())
    except FileNotFoundError:
        return
    except json.JSONDecodeError as exc:
        raise SystemExit(
            f"Invalid gem5 patch state {PATCH_STATE_FILE}: {exc}") from exc

    recorded_targets = patch_state.get("installed_targets", {})
    installed_targets = installed_patch_target_hashes()
    drifted_targets = {
        path: {
            "expected": expected,
            "actual": installed_targets.get(path),
        }
        for path, expected in recorded_targets.items()
        if installed_targets.get(path) != expected
    }
    if drifted_targets:
        raise SystemExit(
            "Installed gem5 patched sources differ from the verified patch "
            f"state: {drifted_targets}. Run setup_gem5.py --clean and "
            "reinstall.")


# =============================================================================
# Utility Functions
# =============================================================================

class Logger:
    """Colored terminal logger."""
    BLUE = "\033[0;34m"
    GREEN = "\033[0;32m"
    YELLOW = "\033[0;33m"
    RED = "\033[0;31m"
    NC = "\033[0m"

    @staticmethod
    def info(msg: str):
        print(f"{Logger.BLUE}[gem5-setup]{Logger.NC} {msg}")

    @staticmethod
    def success(msg: str):
        print(f"{Logger.GREEN}[gem5-setup]{Logger.NC} {msg}")

    @staticmethod
    def warn(msg: str):
        print(f"{Logger.YELLOW}[gem5-setup]{Logger.NC} {msg}")

    @staticmethod
    def error(msg: str):
        print(f"{Logger.RED}[gem5-setup]{Logger.NC} {msg}", file=sys.stderr)

    @staticmethod
    def step(num: int, total: int, msg: str):
        print(f"{Logger.BLUE}[{num}/{total}]{Logger.NC} {msg}")


log = Logger()


def run_cmd(cmd: list, cwd: str = None, check: bool = True,
            capture: bool = False, env: dict = None) -> subprocess.CompletedProcess:
    """Run a command with logging."""
    cmd_str = " ".join(str(c) for c in cmd)
    log.info(f"  $ {cmd_str}")
    merged_env = {**os.environ, **(env or {})}
    return subprocess.run(
        cmd, cwd=cwd, check=check, capture_output=capture,
        text=True, env=merged_env,
    )


def check_prerequisites():
    """Verify required tools are installed."""
    required = {
        "git": "git --version",
        "g++": "g++ --version",
        "python3": "python3 --version",
        "scons": "scons --version",
    }
    missing = []
    for tool, cmd in required.items():
        try:
            subprocess.run(cmd.split(), capture_output=True, check=True)
        except (subprocess.CalledProcessError, FileNotFoundError):
            missing.append(tool)

    if missing:
        log.error(f"Missing prerequisites: {', '.join(missing)}")
        log.error("Install with:")
        log.error("  sudo apt install build-essential git scons python3-dev")
        log.error("  pip install scons  # if not available via apt")
        sys.exit(1)

    # Check GCC version >= 9
    try:
        result = subprocess.run(
            ["g++", "-dumpversion"], capture_output=True, text=True, check=True
        )
        major = int(result.stdout.strip().split(".")[0])
        if major < 9:
            log.warn(f"g++ version {result.stdout.strip()} detected. gem5 requires >= 9.")
    except (subprocess.CalledProcessError, ValueError):
        pass

    log.success("All prerequisites satisfied.")


# =============================================================================
# Core Steps
# =============================================================================

def clone_gem5(tag: str, force: bool = False):
    """Clone gem5 repository at the specified tag."""
    if tag != GEM5_DEFAULT_TAG:
        raise SystemExit(
            "This ECG artifact is pinned to gem5 "
            f"{GEM5_DEFAULT_TAG} ({GEM5_DEFAULT_COMMIT}); "
            f"unsupported --tag {tag!r}.")
    if GEM5_DIR.exists() and not force:
        log.info(f"gem5 already cloned at {GEM5_DIR}")
        actual = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=GEM5_DIR, capture_output=True, text=True, check=True,
        ).stdout.strip()
        if tag == GEM5_DEFAULT_TAG and actual != GEM5_DEFAULT_COMMIT:
            raise SystemExit(
                "gem5 revision mismatch: "
                f"expected {GEM5_DEFAULT_COMMIT}, got {actual}. "
                "Use --clean and rerun setup.")
        VERSION_FILE.write_text(
            json.dumps({
                "tag": tag,
                "commit": actual,
            }, indent=2, sort_keys=True) + "\n")
        log.success(f"gem5 revision verified ({actual}).")
        return

    log.info(f"Cloning gem5 ({tag}) into {GEM5_DIR}...")
    log.info("This may take a few minutes (~300 MB download).")

    if force and GEM5_DIR.exists():
        shutil.rmtree(GEM5_DIR)
        PATCH_STATE_FILE.unlink(missing_ok=True)

    run_cmd([
        "git", "clone", "--depth", "1", "--branch", tag,
        GEM5_REPO_URL, str(GEM5_DIR),
    ])

    actual = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=GEM5_DIR, capture_output=True, text=True, check=True,
    ).stdout.strip()
    if tag == GEM5_DEFAULT_TAG and actual != GEM5_DEFAULT_COMMIT:
        raise SystemExit(
            f"cloned gem5 revision {actual}, expected {GEM5_DEFAULT_COMMIT}")
    VERSION_FILE.write_text(
        json.dumps({
            "tag": tag,
            "commit": actual,
        }, indent=2, sort_keys=True) + "\n")
    log.success(f"gem5 cloned successfully ({actual}).")


def apply_overlays():
    """Copy overlay source files into the cloned gem5 tree."""
    log.info("Applying GraphBrew overlay files to gem5/src/...")
    applied = 0

    for src_rel, dst_rel in OVERLAY_FILE_MAP.items():
        src = OVERLAYS_DIR / src_rel
        dst = GEM5_DIR / "src" / dst_rel

        if not src.exists():
            raise SystemExit(f"Required gem5 overlay missing: {src}")

        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        applied += 1

    log.success(f"Applied {applied} overlay files.")


def apply_patches():
    """Apply SConscript patches to register new SimObjects."""
    log.info("Applying SConscript patches...")

    for patch_rel in PATCH_FILES:
        patch_file = OVERLAYS_DIR / patch_rel
        if not patch_file.exists():
            raise SystemExit(f"Required gem5 SConscript patch missing: {patch_file}")

        # Determine target SConscript
        patch_dir = Path(patch_rel).parent
        target_sconscript = GEM5_DIR / "src" / patch_dir / "SConscript"

        if not target_sconscript.exists():
            raise SystemExit(f"Required gem5 SConscript missing: {target_sconscript}")

        # Read patch content (simple append-style patches)
        patch_content = patch_file.read_text()
        current_content = target_sconscript.read_text()

        if patch_rel == "mem/cache/replacement_policies/SConscript.patch":
            old_simobject = (
                "SimObject('GraphReplacementPolicies.py', sim_objects=[\n"
                "    'GraphGraspRP', 'GraphPoptRP', 'GraphEcgRP'])")
            new_simobject = (
                "SimObject('GraphReplacementPolicies.py', sim_objects=[\n"
                "    'GraphHawkeyeRP', 'GraphGraspRP', 'GraphPoptRP', "
                "'GraphEcgRP'])")
            if old_simobject in current_content and new_simobject not in current_content:
                current_content = current_content.replace(
                    old_simobject, new_simobject)
                target_sconscript.write_text(current_content)

        if patch_rel == "mem/cache/prefetch/SConscript.patch":
            old_simobject = "SimObject('GraphPrefetchers.py', sim_objects=['GraphDropletPrefetcher'])"
            new_simobject = "SimObject('GraphPrefetchers.py', sim_objects=['GraphDropletPrefetcher', 'GraphEcgPfxPrefetcher'])"
            if old_simobject in current_content and new_simobject not in current_content:
                current_content = current_content.replace(old_simobject, new_simobject)
                target_sconscript.write_text(current_content)

        patch_lines = [line for line in patch_content.splitlines() if line.strip()]
        missing_lines = [line for line in patch_lines if line not in current_content]
        if not missing_lines:
            log.info(f"  Patch already applied: {patch_dir}/SConscript")
            continue

        # Append patch content
        marker = "# --- GraphBrew graph-aware policies ---"
        with open(target_sconscript, "a") as f:
            f.write(f"\n{marker}\n")
            f.write("\n".join(missing_lines) + "\n")

        log.success(f"  Patched: {patch_dir}/SConscript")


def apply_unified_diff_patches():
    """Apply unified-diff patches (via `patch -p1`) under gem5/.

    Each patch is checked for idempotency via `patch --dry-run` before
    actually applying. Re-runs are safe and produce no-op output if the
    patch is already applied.
    """
    import subprocess
    log.info("Applying unified-diff patches under gem5/...")
    try:
        patch_state = json.loads(PATCH_STATE_FILE.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        patch_state = {}
    recorded_digests = patch_state.get("patches", {})

    for overlay_rel, target_dir in UNIFIED_DIFF_PATCHES:
        patch_file = OVERLAYS_DIR / overlay_rel
        if not patch_file.exists():
            raise SystemExit(f"Required gem5 patch file missing: {patch_file}")

        target = GEM5_DIR / target_dir
        if not target.exists():
            raise SystemExit(f"Required gem5 patch target missing: {target}")

        patch_digest = hashlib.sha256(patch_file.read_bytes()).hexdigest()
        recorded_digest = recorded_digests.get(overlay_rel)
        if recorded_digest and recorded_digest != patch_digest:
            raise SystemExit(
                "Tracked gem5 patch changed after installation: "
                f"{overlay_rel}. Run setup_gem5.py --clean and reinstall.")

        marker_targets = {
            "arch/riscv/ecg_csr.patch": (
                target / "src/arch/riscv/regs/misc.hh",
                "CSR_ECG_RECORD_FORMAT",
            ),
            "arch/riscv/vector_vtype_guard.patch": (
                target / "src/arch/riscv/isa/formats/vector_conf.isa",
                "GRAPHBREW-RISCV-VTYPE-GUARD",
            ),
            "mem/cache/prefetch/queued_hh.patch": (
                target / "src/mem/cache/prefetch/queued.hh",
                "GRAPHBREW-QUEUE-SERVICING-PATCH",
            ),
            "mem/cache/prefetch/queued_cc_latency.patch": (
                target / "src/mem/cache/prefetch/queued.cc",
                "GRAPHBREW-PREFETCH-LATENCY-GUARD",
            ),
            "cpu/exec_context_ecg_producer.patch": (
                target / "src/cpu/exec_context.hh",
                "setEcgLoadHint",
            ),
            "cpu/o3/dyn_inst_ecg_producer.patch": (
                target / "src/cpu/o3/dyn_inst.hh",
                "setEcgLoadHint",
            ),
            "cpu/o3/lsq_ecg_producer.patch": (
                target / "src/cpu/o3/lsq.cc",
                "attachEcgEpoch",
            ),
            "mem/cache/ecg_victim_request.patch": (
                target / "src/mem/cache/replacement_policies/base.hh",
                "setVictimRequest",
            ),
            "mem/cache/mshr_ecg_merge.patch": (
                target / "src/mem/cache/mshr.hh",
                "EcgReuseBindMshrState",
            ),
            "mem/request_flowthrough.patch": (
                target / "src/mem/request.hh",
                "STRUCTURAL_FLOWTHROUGH",
            ),
            "mem/cache/base_flowthrough.patch": (
                target / "src/mem/cache/base.hh",
                "allow_alloc_on_fill",
            ),
            "mem/cache/base_flowthrough_request_flag.patch": (
                target / "src/mem/cache/base.cc",
                "isStructuralFlowThroughAddress",
            ),
            "mem/cache/array_attribution.patch": (
                target / "src/mem/cache/base.hh",
                "structuralFlowThroughMissTargets",
            ),
            "mem/cache/prefetch_flowthrough.patch": (
                target / "src/mem/cache/prefetch/queued.cc",
                "pfInfo.isStructuralFlowThrough()",
            ),
        }
        marker_target = marker_targets.get(overlay_rel)
        if marker_target and marker_target[0].exists():
            if marker_target[1] in marker_target[0].read_text():
                log.info(f"  Patch already applied (marker): {overlay_rel}")
                continue

        # Dry-run forward to see if already applied
        dry_fwd = subprocess.run(
            ["patch", "-p1", "--dry-run", "--silent", "-i", str(patch_file)],
            cwd=target, capture_output=True, text=True,
        )
        if dry_fwd.returncode != 0:
            dry_reverse = subprocess.run(
                [
                    "patch", "-R", "-p1", "--dry-run", "--silent",
                    "-i", str(patch_file),
                ],
                cwd=target, capture_output=True, text=True,
            )
            if dry_reverse.returncode == 0:
                log.info(f"  Patch already applied (reverse-check): {overlay_rel}")
                continue
            raise SystemExit(
                f"Required gem5 patch is neither cleanly applicable nor "
                f"fully installed: {overlay_rel}\n"
                f"forward:\n{dry_fwd.stdout.strip()}\n{dry_fwd.stderr.strip()}\n"
                f"reverse:\n{dry_reverse.stdout.strip()}\n"
                f"{dry_reverse.stderr.strip()}")

        apply = subprocess.run(
            ["patch", "-p1", "-i", str(patch_file)],
            cwd=target, capture_output=True, text=True,
        )
        if apply.returncode == 0:
            log.success(f"  Applied: {overlay_rel}")
        else:
            raise SystemExit(
                f"Required gem5 patch failed: {overlay_rel}\n"
                f"{apply.stdout.strip()}\n{apply.stderr.strip()}")


def apply_current_vertex_pseudo_inst_patch():
    """Patch m5_work_begin to carry GraphBrew graph hints."""
    target = GEM5_DIR / "src" / "sim" / "pseudo_inst.cc"
    if not target.exists():
        log.warn(f"  pseudo_inst.cc not found: {target}")
        return

    content = target.read_text()
    include_line = '#include "mem/cache/replacement_policies/graph_cache_context_gem5.hh"\n'
    include_anchor = '#include "sim/pseudo_inst.hh"\n'
    if include_line not in content:
        if include_anchor not in content:
            log.warn("  Could not locate pseudo_inst include anchor")
        else:
            content = content.replace(include_anchor, include_anchor + "\n" + include_line, 1)

    workbegin_anchor = '    DPRINTF(PseudoInst, "pseudo_inst::workbegin(%i, %i)\\n", workid, threadid);\n'
    # Upgrade the legacy content-based PFX/mask multiplexing. Epoch zero is a
    # valid extraction, so payload bits cannot distinguish a bare target from a
    # full mask; use a dedicated work ID for extraction.
    legacy_pfx_start = (
        "    if (workid == replacement_policy::graph::"
        "GRAPHBREW_ECG_PFX_TARGET_WORK_ID) {\n"
    )
    if legacy_pfx_start in content and "if ((threadid >> 24) != 0)" in content:
        start = content.index(legacy_pfx_start)
        end = content.index("    }\n\n", start) + len("    }\n\n")
        content = (
            content[:start]
            + legacy_pfx_start
            + "        replacement_policy::graph::setPrefetchTargetHint(threadid);\n"
            + "        return;\n"
            + "    }\n\n"
            + content[end:]
        )
    legacy_reuse_plan = (
        "        uint32_t dest_id = static_cast<uint32_t>(threadid & 0xFFFFFFFFULL);\n"
        "        uint16_t epoch1 = static_cast<uint16_t>((threadid >> 32) & 0xFFFFULL);\n"
        "        uint16_t epoch2 = static_cast<uint16_t>((threadid >> 48) & 0xFFFFULL);\n"
        "        replacement_policy::graph::setDecodedEcgExtractHint2(dest_id, epoch1, epoch2);"
    )
    tiered_reuse_plan = (
        "        uint32_t dest_id = static_cast<uint32_t>(threadid & 0xFFFFFFFFULL);\n"
        "        uint8_t tier = static_cast<uint8_t>((threadid >> 32) & 0x3ULL);\n"
        "        uint16_t epoch1 = static_cast<uint16_t>((threadid >> 34) & 0x7FFFULL);\n"
        "        uint16_t epoch2 = static_cast<uint16_t>((threadid >> 49) & 0x7FFFULL);\n"
        "        replacement_policy::graph::setDecodedEcgExtractHint2(dest_id, tier, epoch1, epoch2);"
    )
    if legacy_reuse_plan in content:
        content = content.replace(legacy_reuse_plan, tiered_reuse_plan, 1)
    legacy_reuse_plan_wrapped = (
        "        uint32_t dest_id = static_cast<uint32_t>(threadid & 0xFFFFFFFFULL);\n"
        "        uint16_t epoch1 =\n"
        "            static_cast<uint16_t>((threadid >> 32) & 0xFFFFULL);\n"
        "        uint16_t epoch2 =\n"
        "            static_cast<uint16_t>((threadid >> 48) & 0xFFFFULL);\n"
        "        replacement_policy::graph::setDecodedEcgExtractHint2(\n"
        "            dest_id, epoch1, epoch2);"
    )
    tiered_reuse_plan_wrapped = (
        "        uint32_t dest_id = static_cast<uint32_t>(threadid & 0xFFFFFFFFULL);\n"
        "        uint8_t tier = static_cast<uint8_t>((threadid >> 32) & 0x3ULL);\n"
        "        uint16_t epoch1 =\n"
        "            static_cast<uint16_t>((threadid >> 34) & 0x7FFFULL);\n"
        "        uint16_t epoch2 =\n"
        "            static_cast<uint16_t>((threadid >> 49) & 0x7FFFULL);\n"
        "        replacement_policy::graph::setDecodedEcgExtractHint2(\n"
        "            dest_id, tier, epoch1, epoch2);"
    )
    if legacy_reuse_plan_wrapped in content:
        content = content.replace(
            legacy_reuse_plan_wrapped, tiered_reuse_plan_wrapped, 1)

    hint_blocks = []
    if "GRAPHBREW_SET_CONTEXT_WORK_ID" not in content:
        hint_blocks.append(
            "    if (workid == replacement_policy::graph::GRAPHBREW_SET_CONTEXT_WORK_ID) {\n"
            "        replacement_policy::graph::setCurrentContextHint(threadid);\n"
            "        return;\n"
            "    }\n\n"
        )
    if "GRAPHBREW_SET_VERTEX_WORK_ID" not in content:
        hint_blocks.append(
            "    if (workid == replacement_policy::graph::GRAPHBREW_SET_VERTEX_WORK_ID) {\n"
            "        replacement_policy::graph::setCurrentVertexHint(threadid);\n"
            "        return;\n"
            "    }\n\n"
        )
    if "GRAPHBREW_ECG_PFX_TARGET_WORK_ID" not in content:
        hint_blocks.append(
            "    if (workid == replacement_policy::graph::GRAPHBREW_ECG_PFX_TARGET_WORK_ID) {\n"
            "        replacement_policy::graph::setPrefetchTargetHint(threadid);\n"
            "        return;\n"
            "    }\n\n"
        )
    if "GRAPHBREW_ECG_EXTRACT_MASK_WORK_ID" not in content:
        hint_blocks.append(
            "    if (workid == replacement_policy::graph::GRAPHBREW_ECG_EXTRACT_MASK_WORK_ID) {\n"
            "        uint64_t fat_mask = threadid;\n"
            "        uint32_t dest_id = static_cast<uint32_t>(fat_mask & 0xFFFFFFULL);\n"
            "        uint16_t epoch = static_cast<uint16_t>((fat_mask >> 24) & 0xFFFFULL);\n"
            "        uint32_t pfx_target = static_cast<uint32_t>((fat_mask >> 40) & 0xFFFFFFULL);\n"
            "        replacement_policy::graph::storeEcgMetadataByVertex(dest_id, 0, 0, epoch);\n"
            "        replacement_policy::graph::setDecodedEcgExtractHint(dest_id, 0, 0, 0, epoch);\n"
            "        if (pfx_target != 0) replacement_policy::graph::setPrefetchTargetHint(pfx_target);\n"
            "        return;\n"
            "    }\n\n"
        )
    if "GRAPHBREW_ECG_EXTRACT2_WORK_ID" not in content:
        hint_blocks.append(
            "    if (workid == replacement_policy::graph::GRAPHBREW_ECG_EXTRACT2_WORK_ID) {\n"
            "        uint32_t dest_id = static_cast<uint32_t>(threadid & 0xFFFFFFFFULL);\n"
            "        uint8_t tier = static_cast<uint8_t>((threadid >> 32) & 0x3ULL);\n"
            "        uint16_t epoch1 = static_cast<uint16_t>((threadid >> 34) & 0x7FFFULL);\n"
            "        uint16_t epoch2 = static_cast<uint16_t>((threadid >> 49) & 0x7FFFULL);\n"
            "        replacement_policy::graph::setDecodedEcgExtractHint2(dest_id, tier, epoch1, epoch2);\n"
            "        return;\n"
            "    }\n\n"
        )
    if "GRAPHBREW_ECG_PFX_TARGET_EPOCH_WORK_ID" not in content:
        # Path A: threadid = target | epoch<<32 | context<<48.
        # Record the candidate epoch in the bounded in-flight buffer (so the
        # prefetched line is stamped at fill) and push the bare target. No
        # single-slot touch (no demand-epoch corruption), no 24-bit truncation.
        hint_blocks.append(
            "    if (workid == replacement_policy::graph::GRAPHBREW_ECG_PFX_TARGET_EPOCH_WORK_ID) {\n"
            "        uint32_t pfxa_target = static_cast<uint32_t>(threadid & 0xFFFFFFFFULL);\n"
            "        uint16_t pfxa_epoch = static_cast<uint16_t>((threadid >> 32) & 0xFFFFULL);\n"
            "        uint16_t pfxa_context = static_cast<uint16_t>((threadid >> 48) & 0xFFFFULL);\n"
            "        replacement_policy::graph::recordPendingPrefetchEpoch(\n"
            "            pfxa_target, pfxa_epoch, pfxa_context);\n"
            "        replacement_policy::graph::setPrefetchTargetHint(pfxa_target);\n"
            "        return;\n"
            "    }\n\n"
        )
    if hint_blocks:
        insertion = "".join(hint_blocks)
        if workbegin_anchor in content:
            content = content.replace(workbegin_anchor, workbegin_anchor + insertion, 1)
        elif "GRAPHBREW_SET_VERTEX_WORK_ID" in content:
            content = content.replace(
                "        replacement_policy::graph::setCurrentVertexHint(threadid);\n"
                "        return;\n"
                "    }\n\n",
                "        replacement_policy::graph::setCurrentVertexHint(threadid);\n"
                "        return;\n"
                "    }\n\n" + insertion,
                1,
            )
        else:
            log.warn("  Could not locate workbegin patch anchor")

    target.write_text(content)
    log.success("  Patched sim/pseudo_inst.cc for GraphBrew current-vertex hints.")


def insert_once(content: str, anchor: str, insertion: str, label: str) -> tuple[str, bool]:
    if insertion.strip() in content:
        return content, False
    if anchor not in content:
        log.warn(f"  Could not locate {label} patch anchor")
        return content, False
    return content.replace(anchor, anchor + insertion, 1), True


def verify_installation_postconditions():
    """Fail if any required ECG overlay or patch is absent."""
    failures = []
    for src_rel, dst_rel in OVERLAY_FILE_MAP.items():
        src = OVERLAYS_DIR / src_rel
        dst = GEM5_DIR / "src" / dst_rel
        if not src.exists() or not dst.exists() or src.read_bytes() != dst.read_bytes():
            failures.append(f"overlay mismatch: {src_rel}")

    marker_checks = {
        GEM5_DIR / "src/mem/request.hh": [
            "ECG_FLOWTHROUGH",
            "STRUCTURAL_FLOWTHROUGH",
        ],
        GEM5_DIR / "src/mem/cache/base.cc": [
            "Request::ECG_FLOWTHROUGH",
            "GEM5_ECG_FLOWTHROUGH_REQUEST_BOUND",
            "isStructuralFlowThroughAddress",
            "allocOnFill(pkt->cmd) && !flowthrough",
            "allocateMissBuffer(pkt, forward_time, true, !flowthrough)",
            "recordGraphArrayMshrMiss",
            "ecgArrayDemandReadBytes",
            "structuralFlowThroughMissTargets",
        ],
        GEM5_DIR / "src/mem/cache/base.hh": [
            "allow_alloc_on_fill",
            "ecgArrayDemandMisses",
            "structuralFlowThroughMissTargets",
            "recordGraphArrayDemandMiss(pkt);",
        ],
        GEM5_DIR / "src/mem/cache/prefetch/base.cc": [
            "flowThrough(pkt->req->getFlags()",
            "structuralFlowThrough(",
        ],
        GEM5_DIR / "src/mem/cache/prefetch/base.hh": [
            "isFlowThrough",
            "bool flowThrough",
        ],
        GEM5_DIR / "src/mem/cache/prefetch/queued.cc": [
            "pfInfo.isFlowThrough()",
            "pfInfo.isStructuralFlowThrough()",
            "GRAPHBREW-PREFETCH-LATENCY-GUARD",
        ],
        GEM5_DIR / "src/mem/cache/prefetch/queued.hh": [
            "GRAPHBREW-QUEUE-SERVICING-PATCH",
            "pfqMissingTranslation.empty()",
        ],
        GEM5_DIR / "src/cpu/exec_context.hh": [
            "setEcgLoadHint",
        ],
        GEM5_DIR / "src/cpu/o3/dyn_inst.hh": [
            "setEcgLoadHint",
        ],
        GEM5_DIR / "src/cpu/o3/lsq.cc": [
            "attachEcgEpoch",
        ],
        GEM5_DIR / "src/sim/pseudo_inst.cc": [
            "GRAPHBREW_SET_CONTEXT_WORK_ID",
            "GRAPHBREW_SET_VERTEX_WORK_ID",
            "GRAPHBREW_ECG_EXTRACT2_WORK_ID",
        ],
        GEM5_DIR / "src/arch/riscv/isa/decoder.isa": [
            "ecg_flow_load",
            "ecg_flow_load_compact",
            "ecg_plan_load",
            "ecg_bind_iload_compact",
        ],
        GEM5_DIR / "src/arch/riscv/regs/misc.hh": [
            "CSR_ECG_RECORD_FORMAT",
        ],
        GEM5_DIR / "src/arch/riscv/isa/formats/vector_conf.isa": [
            "GRAPHBREW-RISCV-VTYPE-GUARD",
            "invalidVsew ? 0 : getSew",
        ],
        GEM5_DIR / "src/mem/cache/replacement_policies/SConscript": [
            "hawkeye_rp.cc",
            "grasp_rp.cc",
            "popt_rp.cc",
            "ecg_rp.cc",
        ],
        GEM5_DIR / "src/mem/cache/replacement_policies/GraphReplacementPolicies.py": [
            "GraphHawkeyeRP",
        ],
        GEM5_DIR / "src/mem/cache/prefetch/SConscript": [
            "droplet.cc",
            "ecg_pfx.cc",
        ],
    }
    for path, markers in marker_checks.items():
        if not path.exists():
            failures.append(f"missing patched file: {path}")
            continue
        text = path.read_text(errors="ignore")
        for marker in markers:
            if marker not in text:
                failures.append(f"missing marker {marker!r} in {path}")

    if failures:
        raise SystemExit(
            "gem5 ECG installation postcondition failure:\n  - " +
            "\n  - ".join(failures))
    log.success("All required gem5 ECG overlay/patch postconditions verified.")
    PATCH_STATE_FILE.write_text(json.dumps({
        "gem5_commit": GEM5_DEFAULT_COMMIT,
        "patches": {
            overlay_rel: hashlib.sha256(
                (OVERLAYS_DIR / overlay_rel).read_bytes()).hexdigest()
            for overlay_rel, _target_dir in UNIFIED_DIFF_PATCHES
        },
        "installed_targets": installed_patch_target_hashes(),
    }, indent=2, sort_keys=True) + "\n")
    log.success(f"Wrote verified gem5 patch state: {PATCH_STATE_FILE}")


def apply_riscv_ecg_extract_patch():
    """Patch RISC-V ISA description for GraphBrew ecg.extract scaffold."""
    isa_dir = GEM5_DIR / "src" / "arch" / "riscv" / "isa"
    formats_path = isa_dir / "formats" / "formats.isa"
    includes_path = isa_dir / "includes.isa"
    decoder_path = isa_dir / "decoder.isa"
    snippet_path = OVERLAYS_DIR / "arch" / "riscv" / "isa" / "decoder_ecg_extract.isa"

    if not formats_path.exists() or not includes_path.exists() or not decoder_path.exists():
        log.warn("  RISC-V ISA files not found; skipping ECG extract patch")
        return

    formats = formats_path.read_text()
    formats, changed = insert_once(
        formats,
        '##include "m5ops.isa"\n',
        '##include "ecg.isa"\n',
        "RISC-V formats include",
    )
    if changed:
        formats_path.write_text(formats)

    includes = includes_path.read_text()
    includes, changed = insert_once(
        includes,
        '#include "sim/pseudo_inst.hh"\n',
        '#include "mem/cache/replacement_policies/graph_cache_context_gem5.hh"\n',
        "RISC-V exec include",
    )
    if changed:
        includes_path.write_text(includes)

    if not snippet_path.exists():
        log.warn(f"  RISC-V ECG decoder snippet not found: {snippet_path}")
        return
    snippet = snippet_path.read_text()
    decoder = decoder_path.read_text()
    marker = "        // GraphBrew ECG custom-0 instruction space.\n"
    load_anchor = "        0x00: decode FUNCT3 {\n"
    marker_pos = decoder.find(marker)
    load_pos = decoder.find(load_anchor, marker_pos if marker_pos >= 0 else 0)
    if marker_pos >= 0 and load_pos > marker_pos:
        decoder = decoder[:marker_pos] + decoder[load_pos:]
    decoder, changed = insert_once(
        decoder,
        '    0x3: decode OPCODE5 {\n',
        snippet,
        "RISC-V custom-0 decoder",
    )
    if changed:
        decoder_path.write_text(decoder)
    log.success("  Patched RISC-V ecg.extract custom-0 scaffold.")


def build_gem5(isas: list, build_type: str, jobs: int):
    """Build gem5 for the specified ISAs."""
    for isa in isas:
        target = f"build/{isa}/gem5.{build_type}"
        binary = GEM5_DIR / target

        if binary.exists():
            log.info(f"Incrementally rebuilding gem5: {target}")
        else:
            log.info(f"Building gem5 for {isa} ({build_type})...")
            log.info(f"This will take 10-30 minutes with {jobs} jobs.")

        run_cmd(
            ["scons", f"-j{jobs}", target],
            cwd=str(GEM5_DIR),
        )

        if binary.exists():
            log.success(f"Built: {target}")
        else:
            log.error(f"Build failed: {target} not found after scons.")
            sys.exit(1)


def verify_build(isas: list, build_type: str):
    """Verify gem5 binaries are functional."""
    for isa in isas:
        binary = GEM5_DIR / f"build/{isa}/gem5.{build_type}"
        if not binary.exists():
            log.warn(f"Binary not found: {binary}")
            continue

        result = run_cmd(
            [str(binary), "--help"],
            check=False, capture=True,
        )
        if result.returncode == 0 and "gem5" in result.stdout.lower():
            log.success(f"Verified: {isa}/gem5.{build_type} is functional.")
        else:
            log.warn(f"Verification issue with {isa}/gem5.{build_type}")


def install_riscv_toolchain():
    """Optionally install RISC-V cross-compilation toolchain."""
    try:
        subprocess.run(
            ["riscv64-linux-gnu-gcc", "--version"],
            capture_output=True, check=True,
        )
        log.info("RISC-V cross-compiler already installed.")
        return
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass

    log.info("Installing RISC-V cross-compilation toolchain...")
    log.info("  sudo apt install gcc-riscv64-linux-gnu g++-riscv64-linux-gnu")
    log.warn("Run manually if needed — skipping automatic install for safety.")


def clean_gem5():
    """Remove cloned gem5 directory."""
    if GEM5_DIR.exists():
        log.info(f"Removing {GEM5_DIR}...")
        shutil.rmtree(GEM5_DIR)
        log.success("gem5 directory removed.")
    for marker in (VERSION_FILE, PATCH_STATE_FILE):
        marker.unlink(missing_ok=True)
    log.success("Clean complete.")


def print_summary(isas: list, build_type: str):
    """Print setup summary."""
    print()
    print("=" * 70)
    print("  gem5 Setup Summary for GraphBrew")
    print("=" * 70)
    print(f"  gem5 location:   {GEM5_DIR}")
    print(f"  Version tag:     {VERSION_FILE.read_text().strip() if VERSION_FILE.exists() else 'unknown'}")
    print(f"  ISAs built:      {', '.join(isas)}")
    print(f"  Build type:      {build_type}")
    print()
    print("  Binaries:")
    for isa in isas:
        binary = GEM5_DIR / f"build/{isa}/gem5.{build_type}"
        status = "OK" if binary.exists() else "MISSING"
        print(f"    {isa}: {binary} [{status}]")
    print()
    print("  Overlay files installed:")
    for src_rel in OVERLAY_FILE_MAP:
        dst = GEM5_DIR / "src" / OVERLAY_FILE_MAP[src_rel]
        status = "OK" if dst.exists() else "pending"
        print(f"    {src_rel} [{status}]")
    print()
    print("  Next steps:")
    print("    make gem5-m5ops-pr")
    print("    python3 scripts/experiments/ecg/roi_matrix.py \\")
    print("        --suite gem5 --benchmark pr --policies LRU --no-build")
    print("=" * 70)


# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Setup gem5 for GraphBrew graph-aware cache simulation",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--isa", nargs="+", default=["X86"],
        choices=VALID_ISAS,
        help="ISA targets to build (default: X86)",
    )
    parser.add_argument(
        "--build-type", default="opt",
        choices=VALID_BUILD_TYPES,
        help="gem5 build type (default: opt)",
    )
    parser.add_argument(
        "--tag", default=GEM5_DEFAULT_TAG,
        help=f"Pinned gem5 tag (must remain {GEM5_DEFAULT_TAG})",
    )
    parser.add_argument(
        "--jobs", type=int, default=max(1, os.cpu_count() - 2),
        help="Parallel build jobs (default: nproc-2)",
    )
    parser.add_argument(
        "--skip-build", action="store_true",
        help="Clone and patch only, skip building",
    )
    parser.add_argument(
        "--rebuild", action="store_true",
        help="Force rebuild even if binaries exist",
    )
    parser.add_argument(
        "--clean", action="store_true",
        help="Remove cloned gem5 directory and exit",
    )
    parser.add_argument(
        "--force", action="store_true",
        help="Force re-clone even if gem5 directory exists",
    )

    args = parser.parse_args()

    # Clean mode
    if args.clean:
        clean_gem5()
        return

    total_steps = 6 if not args.skip_build else 4

    # Step 1: Prerequisites
    log.step(1, total_steps, "Checking prerequisites...")
    check_prerequisites()

    # Step 2: Clone
    log.step(2, total_steps, "Cloning gem5...")
    clone_gem5(args.tag, force=args.force)
    verify_existing_patch_state()

    # Step 3: Apply overlays
    log.step(3, total_steps, "Applying overlay files...")
    apply_overlays()

    # Step 4: Apply patches
    log.step(4, total_steps, "Applying SConscript patches...")
    apply_patches()
    apply_current_vertex_pseudo_inst_patch()
    apply_riscv_ecg_extract_patch()
    apply_unified_diff_patches()
    verify_installation_postconditions()

    if not args.skip_build:
        # Step 5: Build
        log.step(5, total_steps, f"Building gem5 ({', '.join(args.isa)})...")

        if args.rebuild:
            # Force rebuild by removing existing binaries
            for isa in args.isa:
                binary = GEM5_DIR / f"build/{isa}/gem5.{args.build_type}"
                if binary.exists():
                    binary.unlink()

        build_gem5(args.isa, args.build_type, args.jobs)

        # Step 6: Verify
        log.step(6, total_steps, "Verifying build...")
        verify_build(args.isa, args.build_type)

        # RISC-V toolchain hint
        if "RISCV" in args.isa:
            install_riscv_toolchain()

    # Summary
    print_summary(args.isa, args.build_type)
    log.success("gem5 setup complete!")


if __name__ == "__main__":
    main()
