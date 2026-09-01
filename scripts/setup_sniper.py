#!/usr/bin/env python3
"""Setup script for Sniper integration with GraphBrew.

This script mirrors the gem5 setup flow at a lighter-weight level:
- clone Sniper into bench/include/sniper_sim/snipersim,
- optionally checkout a pinned ref,
- leave GraphBrew overlay/config files in bench/include/sniper_sim/,
- optionally build Sniper once outside long experiment jobs.

Usage:
    python3 scripts/setup_sniper.py --dry-run
    python3 scripts/setup_sniper.py --skip-build
    python3 scripts/setup_sniper.py --jobs 8
    python3 scripts/setup_sniper.py --clean

The script is intentionally conservative: it does not copy overlays yet because
Sniper policy integration has not started. Overlay application will be added once
we identify the exact Sniper cache/prefetch extension points for the pinned ref.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
SNIPER_SIM_DIR = PROJECT_ROOT / "bench" / "include" / "sniper_sim"
SNIPER_DIR = SNIPER_SIM_DIR / "snipersim"
SNIPER_OVERLAY_DIR = SNIPER_SIM_DIR / "overlays"
SNIPER_CONFIG_DIR = SNIPER_SIM_DIR / "configs"
VERSION_FILE = SNIPER_SIM_DIR / ".sniper_version"
OVERLAY_STATUS_FILE = SNIPER_SIM_DIR / ".sniper_overlays.json"
SNIPER_REPO_URL = "https://github.com/snipersim/snipersim.git"
SNIPER_DEFAULT_REF = "56505e42fd98bca863fac181e769bd3c98d2bb33"


class Logger:
    BLUE = "\033[0;34m"
    GREEN = "\033[0;32m"
    YELLOW = "\033[0;33m"
    RED = "\033[0;31m"
    NC = "\033[0m"

    @staticmethod
    def info(message: str) -> None:
        print(f"{Logger.BLUE}[sniper-setup]{Logger.NC} {message}")

    @staticmethod
    def success(message: str) -> None:
        print(f"{Logger.GREEN}[sniper-setup]{Logger.NC} {message}")

    @staticmethod
    def warn(message: str) -> None:
        print(f"{Logger.YELLOW}[sniper-setup]{Logger.NC} {message}")

    @staticmethod
    def error(message: str) -> None:
        print(f"{Logger.RED}[sniper-setup]{Logger.NC} {message}", file=sys.stderr)


log = Logger()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def command_text(command: list[str]) -> str:
    return " ".join(command)


def run_cmd(command: list[str], cwd: Path | None = None, dry_run: bool = False) -> subprocess.CompletedProcess[str] | None:
    prefix = f"cd {cwd} && " if cwd else ""
    log.info(prefix + command_text(command))
    if dry_run:
        return None
    return subprocess.run(command, cwd=str(cwd) if cwd else None, text=True, check=True)


def git_head(path: Path) -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=str(path),
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def clone_or_update(args: argparse.Namespace) -> None:
    SNIPER_SIM_DIR.mkdir(parents=True, exist_ok=True)
    if SNIPER_DIR.exists():
        log.info(f"Sniper checkout already exists: {SNIPER_DIR}")
        if args.update:
            run_cmd(["git", "fetch", "--tags", "origin"], cwd=SNIPER_DIR, dry_run=args.dry_run)
    else:
        run_cmd(
            ["git", "clone", args.repo, str(SNIPER_DIR)],
            dry_run=args.dry_run,
        )

    if args.ref:
        run_cmd(["git", "checkout", args.ref], cwd=SNIPER_DIR, dry_run=args.dry_run)
    if not args.dry_run and args.ref == SNIPER_DEFAULT_REF:
        actual = git_head(SNIPER_DIR)
        if actual != SNIPER_DEFAULT_REF:
            raise SystemExit(
                "Sniper revision mismatch: "
                f"expected {SNIPER_DEFAULT_REF}, got {actual}")


def write_version(args: argparse.Namespace) -> None:
    if args.dry_run or not SNIPER_DIR.exists():
        return
    data = {
        "created_utc": utc_now(),
        "repo": args.repo,
        "requested_ref": args.ref,
        "head": git_head(SNIPER_DIR),
        "path": str(SNIPER_DIR),
    }
    VERSION_FILE.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    log.success(f"Wrote {VERSION_FILE}")


def build_sniper(args: argparse.Namespace) -> None:
    if args.skip_build:
        log.info("Skipping Sniper build (--skip-build).")
        return
    command = ["make", f"-j{args.jobs}"]
    if args.build_target:
        command.append(args.build_target)
    if args.dry_run:
        log.info(f"Would build Sniper with: {command_text(command)}")
        return
    if not SNIPER_DIR.exists():
        raise SystemExit(f"Sniper checkout missing: {SNIPER_DIR}")
    if not args.skip_deps_check:
        check_host_dependencies()
    run_cmd(command, cwd=SNIPER_DIR)


_DRY_RUN_OVERLAY_TEXT: dict[Path, str] = {}


def _overlay_text(path: Path, dry_run: bool) -> str:
    key = path.resolve()
    if dry_run and key in _DRY_RUN_OVERLAY_TEXT:
        return _DRY_RUN_OVERLAY_TEXT[key]
    return path.read_text()


def _write_overlay_text(path: Path, text: str, dry_run: bool) -> None:
    if dry_run:
        _DRY_RUN_OVERLAY_TEXT[path.resolve()] = text
    else:
        path.write_text(text)


def replace_once(
    path: Path,
    old: str,
    new: str,
    dry_run: bool,
    accepted_markers: list[str] | None = None,
) -> None:
    if not path.exists():
        raise SystemExit(f"Sniper overlay patch target missing: {path}")
    text = _overlay_text(path, dry_run)
    if new in text:
        log.info(f"Overlay patch already present in {path.relative_to(SNIPER_DIR)}")
        return
    if accepted_markers and any(marker in text for marker in accepted_markers):
        log.info(f"Overlay patch already superseded in {path.relative_to(SNIPER_DIR)}")
        return
    if old not in text:
        anchor = " ".join(old.strip().split())[:160]
        raise SystemExit(
            f"Could not apply overlay patch to {path}; expected anchor not found. "
            f"The Sniper checkout may have changed. Anchor: {anchor!r}"
        )
    log.info(f"Patch {path.relative_to(SNIPER_DIR)}")
    _write_overlay_text(path, text.replace(old, new, 1), dry_run)


def migrate_if_present(path: Path, old: str, new: str, dry_run: bool) -> None:
    """Upgrade a previously installed overlay without requiring a pristine anchor."""
    if not path.exists():
        raise SystemExit(f"Sniper overlay migration target missing: {path}")
    text = _overlay_text(path, dry_run)
    if old not in text or new in text:
        return
    log.info(f"Migrate {path.relative_to(SNIPER_DIR)}")
    _write_overlay_text(path, text.replace(old, new, 1), dry_run)


def normalize_context_ready_handler(path: Path, dry_run: bool) -> None:
    """Install exactly one canonical context-ready handler."""
    text = _overlay_text(path, dry_run)
    marker = "if (arg0 == graphbrew::sniper::GRAPHBREW_CONTEXT_READY_WORK_ID)"
    while marker in text:
        marker_pos = text.index(marker)
        start = text.rfind("\n", 0, marker_pos) + 1
        brace = text.index("{", marker_pos)
        depth = 0
        end = brace
        while end < len(text):
            if text[end] == "{":
                depth += 1
            elif text[end] == "}":
                depth -= 1
                if depth == 0:
                    end += 1
                    while end < len(text) and text[end] in " \t":
                        end += 1
                    if end < len(text) and text[end] == "\n":
                        end += 1
                    break
            end += 1
        text = text[:start] + text[end:]

    set_marker = "if (arg0 == graphbrew::sniper::GRAPHBREW_SET_VERTEX_WORK_ID)"
    set_pos = text.find(set_marker)
    if set_pos < 0:
        raise SystemExit(
            f"Could not normalize context-ready handler in {path}; "
            "set-vertex anchor missing.")
    set_start = text.rfind("\n", 0, set_pos) + 1
    indent = text[set_start:set_pos]
    inner = indent + "   "
    canonical = "\n".join([
        indent + marker,
        indent + "{",
        inner + 'const char* ctx_path = std::getenv("SNIPER_GRAPHBREW_CTX");',
        inner + "if (!ctx_path || !ctx_path[0])",
        inner + '   ctx_path = "/tmp/sniper_graphbrew_ctx.json";',
        inner + "auto& ctx = graphbrew::sniper::globalContext();",
        inner + "ctx.loaded = ctx.loadFromSideband(ctx_path);",
        inner + 'const char* require_reref = std::getenv("SNIPER_REQUIRE_POPT_MATRIX");',
        inner + "bool reref_loaded = ctx.rereference.enabled;",
        inner + "if (ctx.loaded && require_reref && require_reref[0] == '1') {",
        inner + '   const char* matrix_path = std::getenv("SNIPER_POPT_MATRIX");',
        inner + "   reref_loaded = matrix_path && matrix_path[0] &&",
        inner + "      ctx.loadRereferenceMatrix(matrix_path);",
        inner + "   if (reref_loaded && ctx.num_regions > 0)",
        inner + "      ctx.rereference.base_address = ctx.regions[0].base_address;",
        inner + "}",
        inner + "std::fprintf(stderr,",
        inner + '   "[ECG-CONTEXT-READY sim=sniper loaded=%d regions=%u reref=%d]\\n",',
        inner + "   ctx.loaded ? 1 : 0, ctx.num_regions, reref_loaded ? 1 : 0);",
        inner + "if (!ctx.loaded)",
        inner + '   std::fprintf(stderr, "[FATAL] Sniper ECG context-ready load failed: %s\\n", ctx_path);',
        inner + "if (require_reref && require_reref[0] == '1' && !reref_loaded)",
        inner + '   std::fprintf(stderr, "[FATAL] Sniper P-OPT matrix-ready load failed\\n");',
        inner + "return ctx.loaded &&",
        inner + "   (!(require_reref && require_reref[0] == '1') || reref_loaded) ? 0 : 1;",
        indent + "}",
        "",
    ])
    text = text[:set_start] + canonical + text[set_start:]
    if text.count(marker) != 1:
        raise SystemExit(
            f"Context-ready handler normalization failed in {path}")
    _write_overlay_text(path, text, dry_run)


def ensure_reuse_plan_bind_magic_handler(path: Path, dry_run: bool) -> None:
    text = _overlay_text(path, dry_run)
    need_bind = "GRAPHBREW_REUSE_PLAN_BIND_WORK_ID" not in text
    need_clear = "GRAPHBREW_REUSE_PLAN_CLEAR_WORK_ID" not in text
    need_certified = "GRAPHBREW_REUSE_PLAN_CERTIFIED_WORK_ID" not in text
    if not need_bind and not need_clear and not need_certified:
        return
    old = """         MagicMarkerType args = { thread_id: thread_id, core_id: core_id, arg0: arg0, arg1: arg1, str: NULL };
         return Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_USER, (UInt64)&args, true /* expect return value */);
"""
    blocks = ""
    if need_bind:
        blocks += """         if (arg0 == graphbrew::sniper::GRAPHBREW_REUSE_PLAN_BIND_WORK_ID)
         {
            graphbrew::sniper::recordBoundReusePlanLoad(
               static_cast<uint32_t>(core_id), arg1);
            return 0;
         }
"""
    if need_clear:
        blocks += """         if (arg0 == graphbrew::sniper::GRAPHBREW_REUSE_PLAN_CLEAR_WORK_ID)
         {
            graphbrew::sniper::clearBoundReusePlanLoad(
               static_cast<uint32_t>(core_id));
            return 0;
         }
"""
    if need_certified:
        blocks += """         if (arg0 == graphbrew::sniper::GRAPHBREW_REUSE_PLAN_CERTIFIED_WORK_ID)
         {
            graphbrew::sniper::finishBoundReusePlanCertification(
               static_cast<uint32_t>(core_id));
            return 0;
         }
"""
    new = blocks + old
    if old not in text:
        raise RuntimeError(
            f"Cannot locate SIM_CMD_USER hook tail in {path}")
    _write_overlay_text(path, text.replace(old, new, 1), dry_run)


def ensure_ecg_context_lifecycle_hooks(path: Path, dry_run: bool) -> None:
    text = _overlay_text(path, dry_run)
    if "beginEcgContext();" not in text:
        old = """      case SIM_CMD_ROI_START:
         Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_ROI_BEGIN, 0);
"""
        new = """      case SIM_CMD_ROI_START:
         graphbrew::sniper::beginEcgContext();
         Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_ROI_BEGIN, 0);
"""
        if old not in text:
            raise RuntimeError(f"Cannot locate ROI-start hook in {path}")
        text = text.replace(old, new, 1)
    if "endEcgContext();" not in text:
        old = """      case SIM_CMD_ROI_END:
         Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_ROI_END, 0);
"""
        new = """      case SIM_CMD_ROI_END:
         Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_ROI_END, 0);
         graphbrew::sniper::endEcgContext();
"""
        if old not in text:
            raise RuntimeError(f"Cannot locate ROI-end hook in {path}")
        text = text.replace(old, new, 1)
    _write_overlay_text(path, text, dry_run)


def overlay_source_files() -> list[Path]:
    if not SNIPER_OVERLAY_DIR.exists():
        raise SystemExit(f"Sniper overlay directory missing: {SNIPER_OVERLAY_DIR}")
    return [
        source for source in sorted(SNIPER_OVERLAY_DIR.rglob("*"))
        if source.is_file() and source.suffix.lower() in {".h", ".hh", ".cc", ".cpp"}
    ]


def copy_overlay_sources(args: argparse.Namespace) -> list[str]:
    copied: list[str] = []
    for source in overlay_source_files():
        relative = source.relative_to(SNIPER_OVERLAY_DIR)
        target = SNIPER_DIR / relative
        log.info(f"Overlay copy {relative}")
        if not args.dry_run:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        copied.append(str(relative))
    if not copied:
        log.warn(f"No overlay source files found under {SNIPER_OVERLAY_DIR}")
    return copied


def install_graphbrew_configs(args: argparse.Namespace) -> list[str]:
    if not SNIPER_CONFIG_DIR.exists():
        log.warn(f"No tracked Sniper config directory found: {SNIPER_CONFIG_DIR}")
        return []
    if not SNIPER_DIR.exists():
        if args.dry_run:
            log.info(f"Would install GraphBrew Sniper configs after cloning {SNIPER_DIR}")
            return []
        raise SystemExit(f"Sniper checkout missing: {SNIPER_DIR}")

    installed: list[str] = []
    for source in sorted(SNIPER_CONFIG_DIR.rglob("*.cfg")):
        relative = source.relative_to(SNIPER_CONFIG_DIR)
        target = SNIPER_DIR / "config" / relative
        log.info(f"Config copy {relative}")
        if not args.dry_run:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        installed.append(str(relative))
    if not installed:
        log.warn(f"No tracked Sniper config files found under {SNIPER_CONFIG_DIR}")
    return installed


def write_overlay_status(copied_files: list[str]) -> None:
    binary = SNIPER_DIR / "lib" / "sniper"
    if not binary.exists():
        raise SystemExit(
            f"Sniper build completed without expected binary: {binary}")
    patched_files = [
        "common/core/memory_subsystem/cache/cache_base.h",
        "common/core/memory_subsystem/cache/cache_set.cc",
        "common/core/memory_subsystem/cache/cache.cc",
        "common/core/memory_subsystem/parametric_dram_directory_msi/nuca_cache.h",
        "common/core/memory_subsystem/parametric_dram_directory_msi/nuca_cache.cc",
        "common/core/memory_subsystem/parametric_dram_directory_msi/prefetcher.cc",
        "common/core/memory_subsystem/pr_l1_pr_l2_dram_directory_msi/dram_directory_cntlr.cc",
        "common/performance_model/queue_model_history_list.cc",
        "common/performance_model/shmem_perf_model.cc",
        "common/system/magic_server.cc",
        "include/sim_api.h",
    ]
    file_hashes = {}
    for relative in [*copied_files, *patched_files]:
        path = SNIPER_DIR / relative
        if not path.exists():
            raise SystemExit(f"Required patched Sniper file missing: {path}")
        file_hashes[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    newest_source_mtime = max(
        (SNIPER_DIR / relative).stat().st_mtime_ns
        for relative in [*copied_files, *patched_files]
    )
    if binary.stat().st_mtime_ns < newest_source_mtime:
        raise SystemExit(
            "Sniper binary is older than installed ECG sources; "
            "run a full setup build before publishing capabilities.")
    data = {
        "created_utc": utc_now(),
        "sniper_head": git_head(SNIPER_DIR) if SNIPER_DIR.exists() else "",
        "policies": ["grasp", "popt", "ecg"],
        "prefetchers": ["droplet", "ecg_pfx"],
        "copied_files": copied_files,
        "patched_files": patched_files,
        "file_hashes": file_hashes,
        "binary": {
            "path": "lib/sniper",
            "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            "size": binary.stat().st_size,
        },
        "patches": [
            "cache_base_replacement_policy_grasp",
            "cache_set_factory_grasp_popt_ecg",
            "cache_insert_prepare_insertion",
            "cache_only_warmup_timing",
            "prefetcher_factory_droplet",
            "magic_user_graphbrew_hints",
        ],
    }
    OVERLAY_STATUS_FILE.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    log.success(f"Wrote {OVERLAY_STATUS_FILE}")


def patch_grasp_overlay(args: argparse.Namespace) -> None:
    cache_dir = SNIPER_DIR / "common" / "core" / "memory_subsystem" / "cache"
    cache_base = cache_dir / "cache_base.h"
    cache_set = cache_dir / "cache_set.cc"
    cache = cache_dir / "cache.cc"

    replace_once(
        cache_base,
        """         SRRIP,
         SRRIP_QBS,
         RANDOM,
""",
        """         SRRIP,
         SRRIP_QBS,
         GRASP,      // GraphBrew graph-aware SRRIP/GRASP replacement
         RANDOM,
""",
        args.dry_run,
        ["POPT,       // GraphBrew P-OPT oracle replacement"],
    )

    replace_once(
        cache_set,
        """#include "cache_set_srrip.h"
#include "cache_set_mplru.h"
""",
        """#include "cache_set_srrip.h"
#include "cache_set_grasp.h"
#include "cache_set_mplru.h"
""",
        args.dry_run,
        ['#include "cache_set_popt.h"'],
    )
    replace_once(
        cache_set,
        """      case CacheBase::SRRIP:
      case CacheBase::SRRIP_QBS:
         return new CacheSetSRRIP(cfgname, core_id, cache_type, associativity, blocksize, dynamic_cast<CacheSetInfoLRU*>(set_info), getNumQBSAttempts(policy, cfgname, core_id), is_tlb_set);

      case CacheBase::RANDOM:
""",
        """      case CacheBase::SRRIP:
      case CacheBase::SRRIP_QBS:
         return new CacheSetSRRIP(cfgname, core_id, cache_type, associativity, blocksize, dynamic_cast<CacheSetInfoLRU*>(set_info), getNumQBSAttempts(policy, cfgname, core_id), is_tlb_set);

      case CacheBase::GRASP:
         return new CacheSetGRASP(cfgname, core_id, cache_type, associativity, blocksize, dynamic_cast<CacheSetInfoLRU*>(set_info), getNumQBSAttempts(policy, cfgname, core_id), is_tlb_set);

      case CacheBase::RANDOM:
""",
        args.dry_run,
        ["case CacheBase::POPT:"],
    )
    replace_once(
        cache_set,
        """      case CacheBase::SRRIP:
      case CacheBase::SRRIP_QBS:
         return new CacheSetInfoLRU(name, cfgname, core_id, associativity, getNumQBSAttempts(policy, cfgname, core_id));
      case CacheBase::MPLRU:
""",
        """      case CacheBase::SRRIP:
      case CacheBase::SRRIP_QBS:
      case CacheBase::GRASP:
         return new CacheSetInfoLRU(name, cfgname, core_id, associativity, getNumQBSAttempts(policy, cfgname, core_id));
      case CacheBase::MPLRU:
""",
        args.dry_run,
        ["case CacheBase::POPT:"],
    )
    replace_once(
        cache_set,
        """   if (policy == "srrip_qbs")
      return CacheBase::SRRIP_QBS;
   if (policy == "random")
""",
        """   if (policy == "srrip_qbs")
      return CacheBase::SRRIP_QBS;
   if (policy == "grasp")
      return CacheBase::GRASP;
   if (policy == "random")
""",
        args.dry_run,
        ['if (policy == "popt")'],
    )

    migrate_if_present(
        cache,
        """\t\tif (auto ecg_set = dynamic_cast<CacheSetECG*>(m_fake_sets[0]))
		{
			ecg_set->prepareInsertion(addr);
		}
""",
        """\t\tif (auto ecg_set = dynamic_cast<CacheSetECG*>(m_fake_sets[0]))
		{
			ecg_set->prepareInsertion(addr, 0);
		}
""",
        args.dry_run,
    )
    migrate_if_present(
        cache,
        """\t\tif (auto ecg_set = dynamic_cast<CacheSetECG*>(m_sets[set_index]))
		{
			ecg_set->prepareInsertion(addr);
		}
""",
        """\t\tif (auto ecg_set = dynamic_cast<CacheSetECG*>(m_sets[set_index]))
		{
			ecg_set->prepareInsertion(addr, set_index);
		}
""",
        args.dry_run,
    )
    replace_once(
        cache,
        """#include "simulator.h"
#include "cache.h"
#include "log.h"
""",
        """#include "simulator.h"
#include "cache.h"
#include "cache_set_grasp.h"
#include "log.h"
""",
        args.dry_run,
        ['#include "cache_set_popt.h"'],
    )
    replace_once(
        cache,
        """\tm_fake_sets[0]->insert(cache_block_info, fill_buff,
							   eviction, evict_block_info, evict_buff, cntlr);
""",
        """\t\tif (auto grasp_set = dynamic_cast<CacheSetGRASP*>(m_fake_sets[0]))
		{
			grasp_set->prepareInsertion(addr);
		}
		m_fake_sets[0]->insert(cache_block_info, fill_buff,
							   eviction, evict_block_info, evict_buff, cntlr);
""",
        args.dry_run,
        ["dynamic_cast<CacheSetPOPT*>(m_fake_sets[0])"],
    )
    replace_once(
        cache,
        """\tm_sets[set_index]->insert(cache_block_info, fill_buff,
								  eviction, evict_block_info, evict_buff, cntlr);
""",
        """\t\tif (auto grasp_set = dynamic_cast<CacheSetGRASP*>(m_sets[set_index]))
		{
			grasp_set->prepareInsertion(addr);
		}
		m_sets[set_index]->insert(cache_block_info, fill_buff,
								  eviction, evict_block_info, evict_buff, cntlr);
""",
        args.dry_run,
        ["dynamic_cast<CacheSetPOPT*>(m_sets[set_index])"],
    )
def patch_popt_overlay(args: argparse.Namespace) -> None:
    cache_dir = SNIPER_DIR / "common" / "core" / "memory_subsystem" / "cache"
    cache_base = cache_dir / "cache_base.h"
    cache_set = cache_dir / "cache_set.cc"
    cache = cache_dir / "cache.cc"

    replace_once(
        cache_base,
        """         GRASP,      // GraphBrew graph-aware SRRIP/GRASP replacement
         RANDOM,
""",
        """         GRASP,      // GraphBrew graph-aware SRRIP/GRASP replacement
         POPT,       // GraphBrew P-OPT oracle replacement
         RANDOM,
""",
        args.dry_run,
        ["ECG,        // GraphBrew ECG hybrid replacement"],
    )
    replace_once(
        cache_set,
        """#include "cache_set_grasp.h"
#include "cache_set_mplru.h"
""",
        """#include "cache_set_grasp.h"
#include "cache_set_popt.h"
#include "cache_set_mplru.h"
""",
        args.dry_run,
        ['#include "cache_set_ecg.h"'],
    )
    replace_once(
        cache_set,
        """      case CacheBase::GRASP:
         return new CacheSetGRASP(cfgname, core_id, cache_type, associativity, blocksize, dynamic_cast<CacheSetInfoLRU*>(set_info), getNumQBSAttempts(policy, cfgname, core_id), is_tlb_set);

      case CacheBase::RANDOM:
""",
        """      case CacheBase::GRASP:
         return new CacheSetGRASP(cfgname, core_id, cache_type, associativity, blocksize, dynamic_cast<CacheSetInfoLRU*>(set_info), getNumQBSAttempts(policy, cfgname, core_id), is_tlb_set);

      case CacheBase::POPT:
         return new CacheSetPOPT(cfgname, core_id, cache_type, associativity, blocksize, dynamic_cast<CacheSetInfoLRU*>(set_info), getNumQBSAttempts(policy, cfgname, core_id), is_tlb_set);

      case CacheBase::RANDOM:
""",
        args.dry_run,
        ["case CacheBase::ECG:"],
    )
    replace_once(
        cache_set,
        """      case CacheBase::SRRIP_QBS:
      case CacheBase::GRASP:
         return new CacheSetInfoLRU(name, cfgname, core_id, associativity, getNumQBSAttempts(policy, cfgname, core_id));
      case CacheBase::MPLRU:
""",
        """      case CacheBase::SRRIP_QBS:
      case CacheBase::GRASP:
      case CacheBase::POPT:
         return new CacheSetInfoLRU(name, cfgname, core_id, associativity, getNumQBSAttempts(policy, cfgname, core_id));
      case CacheBase::MPLRU:
""",
        args.dry_run,
        ["case CacheBase::ECG:"],
    )
    replace_once(
        cache_set,
        """   if (policy == "grasp")
      return CacheBase::GRASP;
   if (policy == "random")
""",
        """   if (policy == "grasp")
      return CacheBase::GRASP;
   if (policy == "popt")
      return CacheBase::POPT;
   if (policy == "random")
""",
        args.dry_run,
        ['if (policy == "ecg")'],
    )

    replace_once(
        cache,
        """#include "cache_set_grasp.h"
#include "log.h"
""",
        """#include "cache_set_grasp.h"
#include "cache_set_popt.h"
#include "log.h"
""",
        args.dry_run,
        ['#include "cache_set_ecg.h"'],
    )
    replace_once(
        cache,
        """\t\tif (auto grasp_set = dynamic_cast<CacheSetGRASP*>(m_fake_sets[0]))
		{
			grasp_set->prepareInsertion(addr);
		}
		m_fake_sets[0]->insert(cache_block_info, fill_buff,
""",
        """\t\tif (auto grasp_set = dynamic_cast<CacheSetGRASP*>(m_fake_sets[0]))
		{
			grasp_set->prepareInsertion(addr);
		}
		if (auto popt_set = dynamic_cast<CacheSetPOPT*>(m_fake_sets[0]))
		{
			popt_set->prepareInsertion(addr);
		}
		m_fake_sets[0]->insert(cache_block_info, fill_buff,
""",
        args.dry_run,
        ["dynamic_cast<CacheSetECG*>(m_fake_sets[0])"],
    )
    replace_once(
        cache,
        """\t\tif (auto grasp_set = dynamic_cast<CacheSetGRASP*>(m_sets[set_index]))
		{
			grasp_set->prepareInsertion(addr);
		}
		m_sets[set_index]->insert(cache_block_info, fill_buff,
""",
        """\t\tif (auto grasp_set = dynamic_cast<CacheSetGRASP*>(m_sets[set_index]))
		{
			grasp_set->prepareInsertion(addr);
		}
		if (auto popt_set = dynamic_cast<CacheSetPOPT*>(m_sets[set_index]))
		{
			popt_set->prepareInsertion(addr);
		}
		m_sets[set_index]->insert(cache_block_info, fill_buff,
""",
        args.dry_run,
        ["dynamic_cast<CacheSetECG*>(m_sets[set_index])"],
    )
def patch_ecg_overlay(args: argparse.Namespace) -> None:
    cache_dir = SNIPER_DIR / "common" / "core" / "memory_subsystem" / "cache"
    nuca_dir = (
        SNIPER_DIR / "common" / "core" / "memory_subsystem" /
        "parametric_dram_directory_msi"
    )
    directory_dir = (
        SNIPER_DIR / "common" / "core" / "memory_subsystem" /
        "pr_l1_pr_l2_dram_directory_msi"
    )
    cache_base = cache_dir / "cache_base.h"
    cache_set = cache_dir / "cache_set.cc"
    cache = cache_dir / "cache.cc"
    nuca_header = nuca_dir / "nuca_cache.h"
    nuca_source = nuca_dir / "nuca_cache.cc"
    directory_source = directory_dir / "dram_directory_cntlr.cc"

    replace_once(
        cache_base,
        """         POPT,       // GraphBrew P-OPT oracle replacement
         RANDOM,
""",
        """         POPT,       // GraphBrew P-OPT oracle replacement
         ECG,        // GraphBrew ECG hybrid replacement
         RANDOM,
""",
        args.dry_run,
        ['return new DropletPrefetcher(configName, core_id);'],
    )
    replace_once(
        cache_set,
        """#include "cache_set_popt.h"
#include "cache_set_mplru.h"
""",
        """#include "cache_set_popt.h"
#include "cache_set_ecg.h"
#include "cache_set_mplru.h"
""",
        args.dry_run,
    ['return new DropletPrefetcher(configName, core_id);'],
    )
    replace_once(
        cache_set,
        """      case CacheBase::POPT:
         return new CacheSetPOPT(cfgname, core_id, cache_type, associativity, blocksize, dynamic_cast<CacheSetInfoLRU*>(set_info), getNumQBSAttempts(policy, cfgname, core_id), is_tlb_set);

      case CacheBase::RANDOM:
""",
        """      case CacheBase::POPT:
         return new CacheSetPOPT(cfgname, core_id, cache_type, associativity, blocksize, dynamic_cast<CacheSetInfoLRU*>(set_info), getNumQBSAttempts(policy, cfgname, core_id), is_tlb_set);

      case CacheBase::ECG:
         return new CacheSetECG(cfgname, core_id, cache_type, associativity, blocksize, dynamic_cast<CacheSetInfoLRU*>(set_info), getNumQBSAttempts(policy, cfgname, core_id), is_tlb_set);

      case CacheBase::RANDOM:
""",
        args.dry_run,
    ["EcgPfxPrefetcher"],
    )
    replace_once(
        cache_set,
        """      case CacheBase::GRASP:
      case CacheBase::POPT:
         return new CacheSetInfoLRU(name, cfgname, core_id, associativity, getNumQBSAttempts(policy, cfgname, core_id));
      case CacheBase::MPLRU:
""",
        """      case CacheBase::GRASP:
      case CacheBase::POPT:
      case CacheBase::ECG:
         return new CacheSetInfoLRU(name, cfgname, core_id, associativity, getNumQBSAttempts(policy, cfgname, core_id));
      case CacheBase::MPLRU:
""",
        args.dry_run,
    )
    replace_once(
        cache_set,
        """   if (policy == "popt")
      return CacheBase::POPT;
   if (policy == "random")
""",
        """   if (policy == "popt")
      return CacheBase::POPT;
   if (policy == "ecg")
      return CacheBase::ECG;
   if (policy == "random")
""",
        args.dry_run,
    )

    replace_once(
        cache,
        """#include "cache_set_popt.h"
#include "log.h"
""",
        """#include "cache_set_popt.h"
#include "cache_set_ecg.h"
#include "log.h"
""",
        args.dry_run,
    )
    replace_once(
        nuca_header,
        """      boost::tuple<SubsecondTime, HitWhere::where_t> read(IntPtr address, Byte* data_buf, SubsecondTime now, ShmemPerf *perf, bool count, bool is_metadata = false);
""",
        """      boost::tuple<SubsecondTime, HitWhere::where_t> read(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now, ShmemPerf *perf, bool count, bool is_metadata = false);
""",
        args.dry_run,
    )
    replace_once(
        nuca_header,
        """      boost::tuple<SubsecondTime, HitWhere::where_t> write(IntPtr address, Byte* data_buf, bool& eviction, IntPtr& evict_address, Byte* evict_buf, SubsecondTime now, bool count, bool is_metadata = false);
""",
        """      boost::tuple<SubsecondTime, HitWhere::where_t> write(IntPtr address, core_id_t requester, Byte* data_buf, bool& eviction, IntPtr& evict_address, Byte* evict_buf, SubsecondTime now, bool count, bool is_metadata = false);
""",
        args.dry_run,
    )
    replace_once(
        nuca_source,
        """#include "shmem_perf.h"
""",
        """#include "shmem_perf.h"
#include "core/memory_subsystem/cache/graph_cache_context_sniper.h"
""",
        args.dry_run,
    )
    replace_once(
        nuca_source,
        """NucaCache::read(IntPtr address, Byte* data_buf, SubsecondTime now, ShmemPerf *perf, bool count, bool is_metadata)
{
   HitWhere::where_t hit_where = HitWhere::MISS;
""",
        """NucaCache::read(IntPtr address, core_id_t requester, Byte* data_buf, SubsecondTime now, ShmemPerf *perf, bool count, bool is_metadata)
{
   graphbrew::sniper::setCurrentNucaRequesterCore(
      static_cast<uint32_t>(requester));
   HitWhere::where_t hit_where = HitWhere::MISS;
""",
        args.dry_run,
        ["NucaCache::read(IntPtr address, core_id_t requester"],
    )
    replace_once(
        nuca_source,
        """NucaCache::write(IntPtr address, Byte* data_buf, bool& eviction, IntPtr& evict_address, Byte* evict_buf, SubsecondTime now, bool count, bool is_metadata)
{
   HitWhere::where_t hit_where = HitWhere::MISS;
""",
        """NucaCache::write(IntPtr address, core_id_t requester, Byte* data_buf, bool& eviction, IntPtr& evict_address, Byte* evict_buf, SubsecondTime now, bool count, bool is_metadata)
{
   graphbrew::sniper::setCurrentNucaRequesterCore(
      static_cast<uint32_t>(requester));
   HitWhere::where_t hit_where = HitWhere::MISS;
""",
        args.dry_run,
["NucaCache::write(IntPtr address, core_id_t requester"],
    )
    replace_once(
        nuca_header,
        """      UInt64 m_reads, m_writes, m_read_misses, m_write_misses;
""",
        """      UInt64 m_reads, m_writes, m_read_misses, m_write_misses;
      UInt64 m_flowthrough_reads, m_flowthrough_writes;
""",
        args.dry_run,
        ["m_flowthrough_reads"],
    )
    replace_once(
        nuca_source,
        """   , m_write_misses(0)
{
""",
        """   , m_write_misses(0)
   , m_flowthrough_reads(0)
   , m_flowthrough_writes(0)
{
""",
        args.dry_run,
        ["m_flowthrough_reads(0)"],
    )
    replace_once(
        nuca_source,
        """   registerStatsMetric("nuca-cache", m_core_id, "write-misses", &m_write_misses);
}
""",
        """   registerStatsMetric("nuca-cache", m_core_id, "write-misses", &m_write_misses);
   registerStatsMetric("nuca-cache", m_core_id, "flowthrough-reads", &m_flowthrough_reads);
   registerStatsMetric("nuca-cache", m_core_id, "flowthrough-writes", &m_flowthrough_writes);
}
""",
        args.dry_run,
        ['"flowthrough-reads"'],
    )
    migrate_if_present(
        nuca_source,
        """   HitWhere::where_t hit_where = HitWhere::MISS;
   perf->updateTime(now);
   if (graphbrew::sniper::isEcgFlowThroughAddress(
           static_cast<uint64_t>(address)))
   {
      ++m_flowthrough_reads;
      if (count) {
         ++m_reads;
         ++m_read_misses;
      }
      return boost::tuple<SubsecondTime, HitWhere::where_t>(
         SubsecondTime::Zero(), HitWhere::MISS);
   }

   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
""",
        """   HitWhere::where_t hit_where = HitWhere::MISS;
   perf->updateTime(now);
   const bool flowthrough =
      graphbrew::sniper::isEcgFlowThroughAddress(
         static_cast<uint64_t>(address));

   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
""",
        args.dry_run,
    )
    migrate_if_present(
        nuca_source,
        """   HitWhere::where_t hit_where = HitWhere::MISS;
   if (graphbrew::sniper::isEcgFlowThroughAddress(
           static_cast<uint64_t>(address)))
   {
      eviction = false;
      ++m_flowthrough_writes;
      if (count) {
         ++m_writes;
         ++m_write_misses;
      }
      return boost::tuple<SubsecondTime, HitWhere::where_t>(
         SubsecondTime::Zero(), HitWhere::MISS);
   }

   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
""",
        """   HitWhere::where_t hit_where = HitWhere::MISS;
   const bool flowthrough =
      graphbrew::sniper::isEcgFlowThroughAddress(
         static_cast<uint64_t>(address));

   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
""",
        args.dry_run,
    )
    replace_once(
        nuca_source,
        """   HitWhere::where_t hit_where = HitWhere::MISS;
   perf->updateTime(now);

   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
""",
        """   HitWhere::where_t hit_where = HitWhere::MISS;
   perf->updateTime(now);
   const bool flowthrough =
      graphbrew::sniper::isEcgFlowThroughAddress(
         static_cast<uint64_t>(address));

   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
""",
        args.dry_run,
        [
            "perf->updateTime(now);\n   const bool flowthrough =",
            "perf->updateTime(now);\n   const bool structural_flowthrough =",
        ],
    )
    replace_once(
        nuca_source,
        """   else
   {
      if (count) ++m_read_misses;
   }
""",
        """   else
   {
      if (flowthrough) ++m_flowthrough_reads;
      if (count) ++m_read_misses;
   }
""",
        args.dry_run,
        ["if (flowthrough) ++m_flowthrough_reads;"],
    )
    replace_once(
        nuca_source,
        """   HitWhere::where_t hit_where = HitWhere::MISS;

   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
""",
        """   HitWhere::where_t hit_where = HitWhere::MISS;
   const bool flowthrough =
      graphbrew::sniper::isEcgFlowThroughAddress(
         static_cast<uint64_t>(address));

   PrL1CacheBlockInfo* block_info = (PrL1CacheBlockInfo*)m_cache->peekSingleLine(address);
""",
        args.dry_run,
        [
            "HitWhere::where_t hit_where = HitWhere::MISS;\n   const bool flowthrough =",
            "HitWhere::where_t hit_where = HitWhere::MISS;\n   const bool structural_flowthrough =",
        ],
    )
    replace_once(
        nuca_source,
        """   else
   {
      PrL1CacheBlockInfo evict_block_info;

      m_cache->insertSingleLine(address, data_buf,
""",
        """   else
   {
      if (flowthrough)
      {
         eviction = false;
         ++m_flowthrough_writes;
         if (count) ++m_write_misses;
         if (count) ++m_writes;
         return boost::tuple<SubsecondTime, HitWhere::where_t>(
            latency, HitWhere::MISS);
      }
      PrL1CacheBlockInfo evict_block_info;

      m_cache->insertSingleLine(address, data_buf,
""",
        args.dry_run,
        ["if (flowthrough)\n      {\n         eviction = false;"],
    )
    replace_once(
        nuca_header,
        """      UInt64 m_flowthrough_reads, m_flowthrough_writes;
""",
        """      UInt64 m_flowthrough_reads, m_flowthrough_writes;
      UInt64 m_structural_flowthrough_reads, m_structural_flowthrough_writes;
""",
        args.dry_run,
        ["m_structural_flowthrough_reads"],
    )
    replace_once(
        nuca_source,
        """   , m_flowthrough_writes(0)
{
""",
        """   , m_flowthrough_writes(0)
   , m_structural_flowthrough_reads(0)
   , m_structural_flowthrough_writes(0)
{
""",
        args.dry_run,
        ["m_structural_flowthrough_reads(0)"],
    )
    replace_once(
        nuca_source,
        """   registerStatsMetric("nuca-cache", m_core_id, "flowthrough-writes", &m_flowthrough_writes);
}
""",
        """   registerStatsMetric("nuca-cache", m_core_id, "flowthrough-writes", &m_flowthrough_writes);
   registerStatsMetric("nuca-cache", m_core_id, "structural-flowthrough-reads", &m_structural_flowthrough_reads);
   registerStatsMetric("nuca-cache", m_core_id, "structural-flowthrough-writes", &m_structural_flowthrough_writes);
}
""",
        args.dry_run,
        ['"structural-flowthrough-reads"'],
    )
    replace_once(
        nuca_source,
        """   perf->updateTime(now);
   const bool flowthrough =
      graphbrew::sniper::isEcgFlowThroughAddress(
         static_cast<uint64_t>(address));
""",
        """   perf->updateTime(now);
   const bool structural_flowthrough =
      graphbrew::sniper::isStructuralFlowThroughAddress(
         static_cast<uint64_t>(address));
   const bool flowthrough =
      graphbrew::sniper::isEcgFlowThroughAddress(
         static_cast<uint64_t>(address));
""",
        args.dry_run,
        ["perf->updateTime(now);\n   const bool structural_flowthrough ="],
    )
    replace_once(
        nuca_source,
        """   HitWhere::where_t hit_where = HitWhere::MISS;
   const bool flowthrough =
      graphbrew::sniper::isEcgFlowThroughAddress(
         static_cast<uint64_t>(address));
""",
        """   HitWhere::where_t hit_where = HitWhere::MISS;
   const bool structural_flowthrough =
      graphbrew::sniper::isStructuralFlowThroughAddress(
         static_cast<uint64_t>(address));
   const bool flowthrough =
      graphbrew::sniper::isEcgFlowThroughAddress(
         static_cast<uint64_t>(address));
""",
        args.dry_run,
        ["HitWhere::where_t hit_where = HitWhere::MISS;\n   const bool structural_flowthrough ="],
    )
    replace_once(
        nuca_source,
        """      if (flowthrough) ++m_flowthrough_reads;
""",
        """      if (flowthrough) ++m_flowthrough_reads;
      if (structural_flowthrough) ++m_structural_flowthrough_reads;
""",
        args.dry_run,
        ["if (structural_flowthrough) ++m_structural_flowthrough_reads;"],
    )
    replace_once(
        nuca_source,
        """         ++m_flowthrough_writes;
         if (count) ++m_write_misses;
""",
        """         ++m_flowthrough_writes;
         if (structural_flowthrough) ++m_structural_flowthrough_writes;
         if (count) ++m_write_misses;
""",
        args.dry_run,
        ["if (structural_flowthrough) ++m_structural_flowthrough_writes;"],
    )
    migrate_if_present(
        nuca_source,
        """         if (count) ++m_write_misses;
         if (count) ++m_writes;
         return boost::tuple<SubsecondTime, HitWhere::where_t>(
            latency, HitWhere::MISS);
""",
        """         if (count) ++m_write_misses;
         if (count) ++m_writes;
         graphbrew::sniper::recordEcgPlacementMiss(
            static_cast<uint64_t>(address));
         return boost::tuple<SubsecondTime, HitWhere::where_t>(
            latency, HitWhere::MISS);
""",
        args.dry_run,
    )
    migrate_if_present(
        nuca_source,
        """      PrL1CacheBlockInfo evict_block_info;

      m_cache->insertSingleLine(address, data_buf,
""",
        """      PrL1CacheBlockInfo evict_block_info;

      graphbrew::sniper::recordEcgPlacementMiss(
         static_cast<uint64_t>(address));
      m_cache->insertSingleLine(address, data_buf,
""",
        args.dry_run,
    )
    replace_once(
        directory_source,
        """boost::tie(nuca_latency, hit_where) = m_nuca_cache->read(address, nuca_data_buf, getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), orig_shmem_msg->getPerf(), true,orig_shmem_msg->getBlockType());
""",
        """boost::tie(nuca_latency, hit_where) = m_nuca_cache->read(address, orig_shmem_msg->getRequester(), nuca_data_buf, getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), orig_shmem_msg->getPerf(), true,orig_shmem_msg->getBlockType());
""",
        args.dry_run,
        ["m_nuca_cache->read(address, orig_shmem_msg->getRequester()"],
    )
    replace_once(
        directory_source,
        """      m_nuca_cache->write(
         address, data_buf,
""",
        """      m_nuca_cache->write(
         address, requester, data_buf,
""",
        args.dry_run,
        ["m_nuca_cache->write(\n         address, requester, data_buf,"],
    )
    replace_once(
        cache,
        """\t\tif (auto popt_set = dynamic_cast<CacheSetPOPT*>(m_fake_sets[0]))
		{
			popt_set->prepareInsertion(addr);
		}
		m_fake_sets[0]->insert(cache_block_info, fill_buff,
""",
        """\t\tif (auto popt_set = dynamic_cast<CacheSetPOPT*>(m_fake_sets[0]))
		{
			popt_set->prepareInsertion(addr);
		}
		if (auto ecg_set = dynamic_cast<CacheSetECG*>(m_fake_sets[0]))
		{
			ecg_set->prepareInsertion(addr, 0);
		}
		m_fake_sets[0]->insert(cache_block_info, fill_buff,
""",
        args.dry_run,
    )
    replace_once(
        cache,
        """\t\tif (auto popt_set = dynamic_cast<CacheSetPOPT*>(m_sets[set_index]))
		{
			popt_set->prepareInsertion(addr);
		}
		m_sets[set_index]->insert(cache_block_info, fill_buff,
""",
        """\t\tif (auto popt_set = dynamic_cast<CacheSetPOPT*>(m_sets[set_index]))
		{
			popt_set->prepareInsertion(addr);
		}
		if (auto ecg_set = dynamic_cast<CacheSetECG*>(m_sets[set_index]))
		{
			ecg_set->prepareInsertion(addr, set_index);
		}
		m_sets[set_index]->insert(cache_block_info, fill_buff,
""",
        args.dry_run,
    )


def patch_droplet_overlay(args: argparse.Namespace) -> None:
    prefetcher = SNIPER_DIR / "common" / "core" / "memory_subsystem" / "parametric_dram_directory_msi" / "prefetcher.cc"
    replace_once(
        prefetcher,
        """#include "a53prefetcher.h"
""",
        """#include "a53prefetcher.h"
#include "droplet_prefetcher.h"
""",
        args.dry_run,
    )
    replace_once(
        prefetcher,
        """   else if (type == "a53prefetcher")
       return new A53Prefetcher(configName, core_id);

   LOG_PRINT_ERROR("Invalid prefetcher type %s", type.c_str());
""",
        """   else if (type == "a53prefetcher")
       return new A53Prefetcher(configName, core_id);
   else if (type == "droplet")
       return new DropletPrefetcher(configName, core_id);

   LOG_PRINT_ERROR("Invalid prefetcher type %s", type.c_str());
""",
        args.dry_run,
        ['return new DropletPrefetcher(configName, core_id);'],
    )


def patch_graphbrew_simuser_overlay(args: argparse.Namespace) -> None:
    magic_server = SNIPER_DIR / "common" / "system" / "magic_server.cc"
    sim_api = SNIPER_DIR / "include" / "sim_api.h"
    sim_api_text = sim_api.read_text()
    old_constraint = ': "=a"(_res) /* output    */'
    new_constraint = ': "=&a"(_res) /* early-clobber: inputs cannot alias RAX */'
    if sim_api_text.count(new_constraint) >= 3:
        log.info("Overlay patch already present in include/sim_api.h")
    elif sim_api_text.count(old_constraint) >= 3:
        log.info("Patch include/sim_api.h (SimMagic0/1/2 early-clobber)")
        if not args.dry_run:
            sim_api.write_text(
                sim_api_text.replace(old_constraint, new_constraint, 3)
            )
    else:
        raise SystemExit(
            "Could not patch include/sim_api.h SimMagic1/2 constraints; "
            "expected three '=a' outputs or three GraphBrew early-clobber outputs."
        )
    old_decode = (
        "uint32_t fl_vertex = static_cast<uint32_t>(arg1 & 0xFFFFFFFFFFFFULL);\n"
        "             uint16_t fl_epoch = static_cast<uint16_t>((arg1 >> 48) & 0xFFFFULL);"
    )
    new_decode = (
        "uint32_t fl_vertex = static_cast<uint32_t>(arg1 & 0xFFFFFFFFULL);\n"
        "             uint16_t fl_epoch = static_cast<uint16_t>((arg1 >> 32) & 0xFFFFULL);"
    )
    migrate_if_present(
        magic_server, old_decode, new_decode, args.dry_run)
    old_reuse_plan_decode = (
        "uint32_t fl_vertex = static_cast<uint32_t>(arg1 & 0xFFFFFFFFULL);\n"
        "            uint16_t fl_epoch1 = static_cast<uint16_t>((arg1 >> 32) & 0xFFFFULL);\n"
        "            uint16_t fl_epoch2 = static_cast<uint16_t>((arg1 >> 48) & 0xFFFFULL);\n"
        "            graphbrew::sniper::recordEcgReusePlan(\n"
        "               static_cast<uint32_t>(core_id), fl_vertex, fl_epoch1, fl_epoch2);"
    )
    new_reuse_plan_decode = (
        "uint32_t fl_vertex = static_cast<uint32_t>(arg1 & 0xFFFFFFFFULL);\n"
        "            uint8_t fl_tier = static_cast<uint8_t>((arg1 >> 32) & 0x3ULL);\n"
        "            uint16_t fl_epoch1 = static_cast<uint16_t>((arg1 >> 34) & 0x7FFFULL);\n"
        "            uint16_t fl_epoch2 = static_cast<uint16_t>((arg1 >> 49) & 0x7FFFULL);\n"
        "            graphbrew::sniper::recordEcgReusePlan(\n"
        "               static_cast<uint32_t>(core_id), fl_vertex, fl_tier,\n"
        "               fl_epoch1, fl_epoch2);"
    )
    migrate_if_present(
        magic_server, old_reuse_plan_decode, new_reuse_plan_decode, args.dry_run)
    migrate_if_present(
        magic_server,
        """ctx.loaded = ctx.loadFromSideband(ctx_path);
            if (!ctx.loaded)
""",
        """ctx.loaded = ctx.loadFromSideband(ctx_path);
            std::fprintf(stderr,
               "[ECG-CONTEXT-READY sim=sniper loaded=%d regions=%u]\\n",
               ctx.loaded ? 1 : 0, ctx.num_regions);
            if (!ctx.loaded)
""",
        args.dry_run,
    )
    migrate_if_present(
        magic_server,
        """std::fprintf(stderr,
               "[ECG-CONTEXT-READY sim=sniper loaded=%d regions=%u]\\n",
               ctx.loaded ? 1 : 0, ctx.num_regions);
            if (!ctx.loaded)
""",
        """const char* require_reref = std::getenv("SNIPER_REQUIRE_POPT_MATRIX");
            bool reref_loaded = ctx.rereference.enabled;
            if (ctx.loaded && require_reref && require_reref[0] == '1') {
               const char* matrix_path = std::getenv("SNIPER_POPT_MATRIX");
               reref_loaded = matrix_path && matrix_path[0] &&
                  ctx.loadRereferenceMatrix(matrix_path);
               if (reref_loaded && ctx.num_regions > 0)
                  ctx.rereference.base_address = ctx.regions[0].base_address;
            }
            std::fprintf(stderr,
               "[ECG-CONTEXT-READY sim=sniper loaded=%d regions=%u reref=%d]\\n",
               ctx.loaded ? 1 : 0, ctx.num_regions, reref_loaded ? 1 : 0);
            if (!ctx.loaded)
""",
        args.dry_run,
    )
    magic_text = _overlay_text(magic_server, args.dry_run)
    migrate_if_present(
        magic_server,
        """         if (arg0 == graphbrew::sniper::GRAPHBREW_SET_VERTEX_WORK_ID)
         {
""",
        """         if (arg0 == graphbrew::sniper::GRAPHBREW_CONTEXT_READY_WORK_ID)
         {
            const char* ctx_path = std::getenv("SNIPER_GRAPHBREW_CTX");
            if (!ctx_path || !ctx_path[0])
               ctx_path = "/tmp/sniper_graphbrew_ctx.json";
            auto& ctx = graphbrew::sniper::globalContext();
            ctx.loaded = ctx.loadFromSideband(ctx_path);
            const char* require_reref = std::getenv("SNIPER_REQUIRE_POPT_MATRIX");
            bool reref_loaded = ctx.rereference.enabled;
            if (ctx.loaded && require_reref && require_reref[0] == '1') {
               const char* matrix_path = std::getenv("SNIPER_POPT_MATRIX");
               reref_loaded = matrix_path && matrix_path[0] &&
                  ctx.loadRereferenceMatrix(matrix_path);
               if (reref_loaded && ctx.num_regions > 0)
                  ctx.rereference.base_address = ctx.regions[0].base_address;
            }
            std::fprintf(stderr,
               "[ECG-CONTEXT-READY sim=sniper loaded=%d regions=%u reref=%d]\\n",
               ctx.loaded ? 1 : 0, ctx.num_regions, reref_loaded ? 1 : 0);
            if (!ctx.loaded)
               std::fprintf(stderr, "[FATAL] Sniper ECG context-ready load failed: %s\\n", ctx_path);
            if (require_reref && require_reref[0] == '1' && !reref_loaded)
               std::fprintf(stderr, "[FATAL] Sniper P-OPT matrix-ready load failed\\n");
            return ctx.loaded &&
               (!(require_reref && require_reref[0] == '1') || reref_loaded) ? 0 : 1;
         }
         if (arg0 == graphbrew::sniper::GRAPHBREW_SET_VERTEX_WORK_ID)
         {
""",
        args.dry_run,
    )
    replace_once(
        magic_server,
        """#include "magic_server.h"
#include "sim_api.h"
""",
        """#include "magic_server.h"
#include "sim_api.h"
#include "core/memory_subsystem/cache/graph_cache_context_sniper.h"
""",
        args.dry_run,
    )
    if "GRAPHBREW_SET_VERTEX_WORK_ID" in magic_text:
       log.info("Overlay patch already present in common/system/magic_server.cc")
    else:
       replace_once(
           magic_server,
           """      case SIM_CMD_USER:
      {
         MagicMarkerType args = { thread_id: thread_id, core_id: core_id, arg0: arg0, arg1: arg1, str: NULL };
         return Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_USER, (UInt64)&args, true /* expect return value */);
      }
""",
           """      case SIM_CMD_USER:
      {
        if (arg0 == graphbrew::sniper::GRAPHBREW_CONTEXT_READY_WORK_ID)
        {
           const char* ctx_path = std::getenv("SNIPER_GRAPHBREW_CTX");
           if (!ctx_path || !ctx_path[0])
              ctx_path = "/tmp/sniper_graphbrew_ctx.json";
           auto& ctx = graphbrew::sniper::globalContext();
           ctx.loaded = ctx.loadFromSideband(ctx_path);
           const char* require_reref = std::getenv("SNIPER_REQUIRE_POPT_MATRIX");
           bool reref_loaded = ctx.rereference.enabled;
           if (ctx.loaded && require_reref && require_reref[0] == '1') {
              const char* matrix_path = std::getenv("SNIPER_POPT_MATRIX");
              reref_loaded = matrix_path && matrix_path[0] &&
                 ctx.loadRereferenceMatrix(matrix_path);
              if (reref_loaded && ctx.num_regions > 0)
                 ctx.rereference.base_address = ctx.regions[0].base_address;
           }
           std::fprintf(stderr,
              "[ECG-CONTEXT-READY sim=sniper loaded=%d regions=%u reref=%d]\\n",
              ctx.loaded ? 1 : 0, ctx.num_regions, reref_loaded ? 1 : 0);
           if (!ctx.loaded)
              std::fprintf(stderr, "[FATAL] Sniper ECG context-ready load failed: %s\\n", ctx_path);
           if (require_reref && require_reref[0] == '1' && !reref_loaded)
              std::fprintf(stderr, "[FATAL] Sniper P-OPT matrix-ready load failed\\n");
           return ctx.loaded &&
              (!(require_reref && require_reref[0] == '1') || reref_loaded) ? 0 : 1;
        }
        if (arg0 == graphbrew::sniper::GRAPHBREW_SET_VERTEX_WORK_ID)
         {
            graphbrew::sniper::setCurrentVertexHint(static_cast<uint32_t>(core_id), arg1);
            return 0;
         }
         if (arg0 == graphbrew::sniper::GRAPHBREW_ECG_PFX_TARGET_WORK_ID)
         {
            graphbrew::sniper::setPrefetchTargetHint(static_cast<uint32_t>(core_id), arg1);
            return 0;
         }
         if (arg0 == graphbrew::sniper::GRAPHBREW_ECG_EXTRACT_WORK_ID)
         {
            uint32_t fl_vertex = static_cast<uint32_t>(arg1 & 0xFFFFFFFFULL);
            uint16_t fl_epoch = static_cast<uint16_t>((arg1 >> 32) & 0xFFFFULL);
            graphbrew::sniper::recordEcgEpoch(static_cast<uint32_t>(core_id), fl_vertex, fl_epoch);
            return 0;
         }
         if (arg0 == graphbrew::sniper::GRAPHBREW_ECG_EXTRACT2_WORK_ID)
         {
            uint32_t fl_vertex = static_cast<uint32_t>(arg1 & 0xFFFFFFFFULL);
            uint8_t fl_tier = static_cast<uint8_t>((arg1 >> 32) & 0x3ULL);
            uint16_t fl_epoch1 = static_cast<uint16_t>((arg1 >> 34) & 0x7FFFULL);
            uint16_t fl_epoch2 = static_cast<uint16_t>((arg1 >> 49) & 0x7FFFULL);
            graphbrew::sniper::recordEcgReusePlan(
               static_cast<uint32_t>(core_id), fl_vertex, fl_tier,
               fl_epoch1, fl_epoch2);
            return 0;
         }
         if (arg0 == graphbrew::sniper::GRAPHBREW_REUSE_PLAN_BIND_WORK_ID)
         {
            graphbrew::sniper::recordBoundReusePlanLoad(
               static_cast<uint32_t>(core_id), arg1);
            return 0;
         }
         if (arg0 == graphbrew::sniper::GRAPHBREW_REUSE_PLAN_CLEAR_WORK_ID)
         {
            graphbrew::sniper::clearBoundReusePlanLoad(
               static_cast<uint32_t>(core_id));
            return 0;
         }
         MagicMarkerType args = { thread_id: thread_id, core_id: core_id, arg0: arg0, arg1: arg1, str: NULL };
         return Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_USER, (UInt64)&args, true /* expect return value */);
      }
""",
           args.dry_run,
)
    replace_once(
magic_server,
"""          if (arg0 == graphbrew::sniper::GRAPHBREW_ECG_EXTRACT_WORK_ID)
  {
            uint32_t fl_vertex = static_cast<uint32_t>(arg1 & 0xFFFFFFFFULL);
            uint16_t fl_epoch = static_cast<uint16_t>((arg1 >> 32) & 0xFFFFULL);
            graphbrew::sniper::recordEcgEpoch(static_cast<uint32_t>(core_id), fl_vertex, fl_epoch);
            return 0;
  }
""",
"""          if (arg0 == graphbrew::sniper::GRAPHBREW_ECG_EXTRACT_WORK_ID)
  {
            uint32_t fl_vertex = static_cast<uint32_t>(arg1 & 0xFFFFFFFFULL);
            uint16_t fl_epoch = static_cast<uint16_t>((arg1 >> 32) & 0xFFFFULL);
            graphbrew::sniper::recordEcgEpoch(static_cast<uint32_t>(core_id), fl_vertex, fl_epoch);
            return 0;
  }
  if (arg0 == graphbrew::sniper::GRAPHBREW_ECG_EXTRACT2_WORK_ID)
  {
            uint32_t fl_vertex = static_cast<uint32_t>(arg1 & 0xFFFFFFFFULL);
            uint8_t fl_tier = static_cast<uint8_t>((arg1 >> 32) & 0x3ULL);
            uint16_t fl_epoch1 = static_cast<uint16_t>((arg1 >> 34) & 0x7FFFULL);
            uint16_t fl_epoch2 = static_cast<uint16_t>((arg1 >> 49) & 0x7FFFULL);
            graphbrew::sniper::recordEcgReusePlan(
               static_cast<uint32_t>(core_id), fl_vertex, fl_tier,
               fl_epoch1, fl_epoch2);
            return 0;
  }
  if (arg0 == graphbrew::sniper::GRAPHBREW_REUSE_PLAN_BIND_WORK_ID)
  {
            graphbrew::sniper::recordBoundReusePlanLoad(
               static_cast<uint32_t>(core_id), arg1);
            return 0;
  }
  if (arg0 == graphbrew::sniper::GRAPHBREW_REUSE_PLAN_CLEAR_WORK_ID)
  {
            graphbrew::sniper::clearBoundReusePlanLoad(
               static_cast<uint32_t>(core_id));
            return 0;
  }
""",
args.dry_run,
["GRAPHBREW_ECG_EXTRACT2_WORK_ID", "GRAPHBREW_REUSE_PLAN_BIND_WORK_ID"],
    )
    ensure_reuse_plan_bind_magic_handler(magic_server, args.dry_run)
    ensure_ecg_context_lifecycle_hooks(magic_server, args.dry_run)
    replace_once(
        magic_server,
        """         if (arg0 == graphbrew::sniper::GRAPHBREW_SET_VERTEX_WORK_ID)
         {
""",
        """         if (arg0 == graphbrew::sniper::GRAPHBREW_CONTEXT_READY_WORK_ID)
         {
            const char* ctx_path = std::getenv("SNIPER_GRAPHBREW_CTX");
            if (!ctx_path || !ctx_path[0])
               ctx_path = "/tmp/sniper_graphbrew_ctx.json";
            auto& ctx = graphbrew::sniper::globalContext();
            ctx.loaded = ctx.loadFromSideband(ctx_path);
            const char* require_reref = std::getenv("SNIPER_REQUIRE_POPT_MATRIX");
            bool reref_loaded = ctx.rereference.enabled;
            if (ctx.loaded && require_reref && require_reref[0] == '1') {
               const char* matrix_path = std::getenv("SNIPER_POPT_MATRIX");
               reref_loaded = matrix_path && matrix_path[0] &&
                  ctx.loadRereferenceMatrix(matrix_path);
               if (reref_loaded && ctx.num_regions > 0)
                  ctx.rereference.base_address = ctx.regions[0].base_address;
            }
            std::fprintf(stderr,
               "[ECG-CONTEXT-READY sim=sniper loaded=%d regions=%u reref=%d]\\n",
               ctx.loaded ? 1 : 0, ctx.num_regions, reref_loaded ? 1 : 0);
            if (!ctx.loaded)
               std::fprintf(stderr, "[FATAL] Sniper ECG context-ready load failed: %s\\n", ctx_path);
            if (require_reref && require_reref[0] == '1' && !reref_loaded)
               std::fprintf(stderr, "[FATAL] Sniper P-OPT matrix-ready load failed\\n");
            return ctx.loaded &&
               (!(require_reref && require_reref[0] == '1') || reref_loaded) ? 0 : 1;
         }
         if (arg0 == graphbrew::sniper::GRAPHBREW_SET_VERTEX_WORK_ID)
         {
""",
        args.dry_run,
        ["GRAPHBREW_CONTEXT_READY_WORK_ID"],
    )
    normalize_context_ready_handler(magic_server, args.dry_run)
    migrate_if_present(
        magic_server,
        """            graphbrew::sniper::setCurrentVertexHint(static_cast<uint32_t>(core_id), arg1);
""",
        """            if (arg1 == ~uint64_t(0))
               graphbrew::sniper::clearCurrentVertexHint(static_cast<uint32_t>(core_id));
            else
               graphbrew::sniper::setCurrentVertexHint(static_cast<uint32_t>(core_id), arg1);
""",
        args.dry_run,
    )


def patch_ecg_pfx_prefetcher_overlay(args: argparse.Namespace) -> None:
    prefetcher = SNIPER_DIR / "common" / "core" / "memory_subsystem" / "parametric_dram_directory_msi" / "prefetcher.cc"
    replace_once(
        prefetcher,
        """#include "droplet_prefetcher.h"
""",
        """#include "droplet_prefetcher.h"
#include "ecg_pfx_prefetcher.h"
""",
        args.dry_run,
    )
    replace_once(
        prefetcher,
        """   else if (type == "droplet")
       return new DropletPrefetcher(configName, core_id);

   LOG_PRINT_ERROR("Invalid prefetcher type %s", type.c_str());
""",
        """   else if (type == "droplet")
       return new DropletPrefetcher(configName, core_id);
   else if (type == "ecg_pfx")
       return new EcgPfxPrefetcher(configName, core_id);

   LOG_PRINT_ERROR("Invalid prefetcher type %s", type.c_str());
""",
        args.dry_run,
        ["EcgPfxPrefetcher"],
    )


def patch_cache_only_history_queue(args: argparse.Namespace) -> None:
    queue_source = (
        SNIPER_DIR / "common" / "performance_model" /
        "queue_model_history_list.cc"
    )
    replace_once(
        queue_source,
        """QueueModelHistoryList::computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester)
{
   LOG_ASSERT_ERROR(m_free_interval_list.size() >= 1,
""",
        """QueueModelHistoryList::computeQueueDelay(SubsecondTime pkt_time, SubsecondTime processing_time, core_id_t requester)
{
   // CACHE_ONLY warming updates cache state without advancing the interval-core
   // clock. Exact-time history queues therefore receive non-monotonic packet
   // timestamps and can exhaust their free-interval list. Queue latency is not
   // consumed in this mode, so leave the detailed-ROI queue state untouched.
   if (Sim()->getInstrumentationMode() == InstMode::CACHE_ONLY)
      return SubsecondTime::Zero();

   LOG_ASSERT_ERROR(m_free_interval_list.size() >= 1,
""",
        args.dry_run,
        ["CACHE_ONLY warming updates cache state"],
    )


def patch_cache_only_shmem_timing(args: argparse.Namespace) -> None:
    shmem_source = (
        SNIPER_DIR / "common" / "performance_model" / "shmem_perf_model.cc"
    )
    replace_once(
        shmem_source,
        """ShmemPerfModel::updateElapsedTime(SubsecondTime time, Thread_t thread_num)
{
   LOG_PRINT("updateElapsedTime: time(%s)", itostr(time).c_str());
""",
        """ShmemPerfModel::updateElapsedTime(SubsecondTime time, Thread_t thread_num)
{
   // CACHE_ONLY leaves shared-memory elapsed time unchanged.
   if (Sim()->getInstrumentationMode() == InstMode::CACHE_ONLY)
      return;

   LOG_PRINT("updateElapsedTime: time(%s)", itostr(time).c_str());
""",
        args.dry_run,
        ["CACHE_ONLY leaves shared-memory elapsed time unchanged"],
    )
    replace_once(
        shmem_source,
        """ShmemPerfModel::incrElapsedTime(SubsecondTime time, Thread_t thread_num)
{
   LOG_PRINT("incrElapsedTime: time(%s)", itostr(time).c_str());
""",
        """ShmemPerfModel::incrElapsedTime(SubsecondTime time, Thread_t thread_num)
{
   // CACHE_ONLY warms cache contents but intentionally does not model time.
   if (Sim()->getInstrumentationMode() == InstMode::CACHE_ONLY)
      return;

   LOG_PRINT("incrElapsedTime: time(%s)", itostr(time).c_str());
""",
        args.dry_run,
        ["CACHE_ONLY warms cache contents"],
    )


def apply_overlays(args: argparse.Namespace) -> list[str]:
    if not args.apply_overlays:
        return []
    if not SNIPER_DIR.exists():
        raise SystemExit(f"Sniper checkout missing: {SNIPER_DIR}")
    log.info("Applying GraphBrew Sniper overlays")
    copied_files = copy_overlay_sources(args)
    patch_grasp_overlay(args)
    patch_popt_overlay(args)
    patch_ecg_overlay(args)
    patch_droplet_overlay(args)
    patch_graphbrew_simuser_overlay(args)
    patch_ecg_pfx_prefetcher_overlay(args)
    patch_cache_only_history_queue(args)
    patch_cache_only_shmem_timing(args)
    if args.dry_run:
        log.info("Overlay application dry-run completed.")
    else:
        OVERLAY_STATUS_FILE.unlink(missing_ok=True)
        log.success("Applied GraphBrew Sniper overlays.")
    return copied_files


def compiler_for_checks() -> str:
    return os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc") or "cc"


def header_available(header: str) -> bool:
    compiler = compiler_for_checks()
    result = subprocess.run(
        [compiler, "-x", "c", "-E", "-"],
        input=f"#include <{header}>\n",
        capture_output=True,
        text=True,
        check=False,
    )
    return result.returncode == 0


def check_host_dependencies() -> None:
    missing_headers = [header for header in ("sqlite3.h",) if not header_available(header)]
    if not missing_headers:
        return
    raise SystemExit(
        "Missing Sniper build dependency headers: "
        + ", ".join(missing_headers)
        + "\nInstall the matching OS packages, e.g. Ubuntu/Debian: "
        + "sudo apt-get install libsqlite3-dev; RHEL/CentOS/Fedora: "
        + "sudo dnf install sqlite-devel. On UVA Slurm, load/use a toolchain "
        + "environment that provides sqlite development headers."
    )


def smoke_test(args: argparse.Namespace) -> None:
    if not args.smoke:
        return
    run_sniper = SNIPER_DIR / "run-sniper"
    if args.dry_run:
        log.info("Would run Sniper smoke test.")
        return
    if not run_sniper.exists():
        raise SystemExit(f"run-sniper not found: {run_sniper}")
    out_dir = Path(args.smoke_dir)
    command = [
        str(run_sniper),
        "-n", "1",
        "--fast-forward",
        "-d", str(out_dir),
        "-cgraphbrew/graph_sniper",
        "--", "/bin/true",
    ]
    run_cmd(command)


def graphbrew_smoke_test(args: argparse.Namespace) -> None:
    if not args.graphbrew_smoke:
        return
    run_sniper = SNIPER_DIR / "run-sniper"
    if args.dry_run:
        log.info("Would build and run GraphBrew Sniper smoke binaries.")
        return
    if not run_sniper.exists():
        raise SystemExit(f"run-sniper not found: {run_sniper}")
    run_cmd(["make", "sniper-hello_roi", "sniper-pr_kernel_smoke", "sniper-bfs_kernel_smoke", "sniper-sssp_kernel_smoke"], cwd=PROJECT_ROOT)
    out_dir = Path(args.graphbrew_smoke_dir)
    command = [
        str(run_sniper),
        "--roi",
        "--no-cache-warming",
        "-n", "1",
        "-d", str(out_dir),
        "-cgraphbrew/graph_sniper",
        "--", str(PROJECT_ROOT / "bench" / "bin_sniper" / "pr_kernel_smoke"),
    ]
    run_cmd(command, cwd=PROJECT_ROOT)
    parser = PROJECT_ROOT / "bench" / "include" / "sniper_sim" / "scripts" / "parse_stats.py"
    if parser.exists():
        run_cmd([sys.executable, str(parser), str(out_dir)], cwd=PROJECT_ROOT)


def clean(args: argparse.Namespace) -> int:
    if args.dry_run:
        log.info(
            f"Would remove {SNIPER_DIR}, {VERSION_FILE}, and "
            f"{OVERLAY_STATUS_FILE}")
        return 0
    if SNIPER_DIR.exists():
        shutil.rmtree(SNIPER_DIR)
    else:
        log.info(f"No Sniper checkout to remove: {SNIPER_DIR}")
    for marker in (VERSION_FILE, OVERLAY_STATUS_FILE):
        marker.unlink(missing_ok=True)
    log.success("Removed Sniper checkout and capability markers.")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Setup the pinned Sniper ECG backend.")
    parser.add_argument("--repo", default=SNIPER_REPO_URL, help="Sniper git repository URL.")
    parser.add_argument("--ref", default=SNIPER_DEFAULT_REF, help="Sniper branch/tag/commit to checkout.")
    parser.add_argument("--jobs", type=int, default=8, help="Parallel build jobs.")
    parser.add_argument("--build-target", default="", help="Optional Sniper make target, e.g. configscripts or standalone.")
    parser.add_argument("--skip-build", action="store_true", help="Clone/checkout only; do not build.")
    parser.add_argument("--skip-deps-check", action="store_true", help="Skip GraphBrew host dependency preflight before building Sniper.")
    parser.add_argument("--update", action="store_true", help="Fetch updates if checkout already exists.")
    parser.add_argument("--apply-overlays", action="store_true", help="Copy tracked GraphBrew overlay files into the Sniper checkout and apply wiring patches.")
    parser.add_argument("--smoke", action="store_true", help="Run a minimal /bin/true Sniper smoke test after build.")
    parser.add_argument("--smoke-dir", default="/tmp/sniper-graphbrew-smoke", help="Smoke-test output directory.")
    parser.add_argument("--graphbrew-smoke", action="store_true", help="Run GraphBrew hello/pr_kernel Sniper smoke tests after build.")
    parser.add_argument("--graphbrew-smoke-dir", default="/tmp/sniper-graphbrew-pr-kernel", help="GraphBrew Sniper smoke output directory.")
    parser.add_argument("--dry-run", action="store_true", help="Print actions without executing them.")
    parser.add_argument("--clean", action="store_true", help="Remove the Sniper checkout and version file.")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.clean:
        return clean(args)
    clone_or_update(args)
    write_version(args)
    if not args.dry_run and (args.apply_overlays or not args.skip_build):
        OVERLAY_STATUS_FILE.unlink(missing_ok=True)
    install_graphbrew_configs(args)
    copied_files = apply_overlays(args)
    build_sniper(args)
    smoke_test(args)
    graphbrew_smoke_test(args)
    if (args.apply_overlays and not args.dry_run and
            not args.skip_build and not args.build_target):
        write_overlay_status(copied_files)
    elif (args.apply_overlays and not args.dry_run and
          not args.skip_build and args.build_target):
        log.warn(
            "Not publishing Sniper capability marker for partial "
            f"build target {args.build_target!r}; rerun without --build-target.")
    log.success("Sniper setup step completed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
