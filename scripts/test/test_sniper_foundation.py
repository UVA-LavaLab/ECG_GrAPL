from pathlib import Path
import os
import runpy
import shutil
import subprocess
import sys
import time
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (ROOT / relative).read_text()


def test_foundation_probe_uses_high_bits_and_real_loads():
    source = read("bench/src_sniper/foundation_probe.cc")
    harness = read("bench/include/sniper_sim/sniper_harness.h")
    assert "0xfedcba9876543210ULL" in source
    assert "volatile uint32_t record4" in source
    assert "volatile uint64_t record8" in source
    assert "const uint32_t loaded4 = record4;" in source
    assert "const uint64_t loaded8 = record8;" in source
    assert "foundation_echo" in harness
    assert "foundation_arm_read" in harness
    assert "foundation_read_status" in harness
    assert "foundation_report_loaded" in harness


def test_foundation_overlay_captures_completed_read_data():
    header = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/graph_cache_context_sniper.h")
    source = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/graph_cache_context_sniper.cc")
    setup = read("scripts/setup_sniper.py")
    assert "foundationObserveRead" in header
    assert "std::memcpy(&actual, data, bytes)" in source
    assert "bytes != armed_bytes" in source
    assert "virtual_address + offset" in source
    assert "data_present=0" in source
    assert "matched=%u" in source
    assert "reinterpret_cast<const uint8_t*>(data_buf)" in setup
    assert "static_cast<uint64_t>(address)" in setup
    assert "static_cast<uint64_t>(physical_address)" in setup


def test_foundation_setup_installs_probe_handlers():
    setup = read("scripts/setup_sniper.py")
    assert "GRAPHBREW_FOUNDATION_ECHO_WORK_ID" in setup
    assert "foundationEcho64" in setup
    assert "foundationReadStatus" in setup
    assert "foundationCommitLoadedValue" in setup


def test_rss_watchdog_uses_explicit_process_ids():
    watchdog = read("scripts/test/sniper_rss_watch.py")
    assert 'Path(f"/proc/{pid}/status")' in watchdog
    assert "os.kill(identity.pid, signal.SIGTERM)" in watchdog
    assert "os.kill(identity.pid, signal.SIGKILL)" in watchdog
    assert "pkill" not in watchdog
    assert "killall" not in watchdog
    assert "stdout=subprocess.PIPE" not in watchdog
    assert "ProcessIdentity" in watchdog
    assert "PR_SET_CHILD_SUBREAPER".lower() in watchdog.lower()


def test_rss_watchdog_drains_verbose_output(tmp_path: Path):
    log = tmp_path / "verbose.log"
    watchdog = ROOT / "scripts/test/sniper_rss_watch.py"
    command = [
        sys.executable, str(watchdog),
        "--rss-mib", "256", "--seconds", "5", "--log", str(log), "--",
        sys.executable, "-c",
        "import sys; sys.stdout.write('x' * (2 * 1024 * 1024))",
    ]
    result = subprocess.run(command, capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "[watchdog_reason] exit" in log.read_text()
    assert log.stat().st_size >= 2 * 1024 * 1024


@pytest.mark.parametrize("flag,value", [
    ("--rss-mib", "0"), ("--seconds", "0"), ("--sample-ms", "0"),
])
def test_rss_watchdog_rejects_unbounded_or_zero_limits(tmp_path: Path, flag, value):
    log = tmp_path / "invalid.log"
    command = [
        sys.executable, str(ROOT / "scripts/test/sniper_rss_watch.py"),
        "--rss-mib", "256", "--seconds", "5", "--sample-ms", "100",
        "--log", str(log), flag, value, "--", sys.executable, "-c", "pass",
    ]
    result = subprocess.run(command, capture_output=True, text=True, timeout=10)
    assert result.returncode == 2
    assert not log.exists()


def test_rss_watchdog_kills_captured_orphan(tmp_path: Path):
    log = tmp_path / "orphan.log"
    pid_file = tmp_path / "child.pid"
    watchdog = ROOT / "scripts/test/sniper_rss_watch.py"
    child_script = (
        "import pathlib,subprocess,sys;"
        "p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)']);"
        f"pathlib.Path({str(pid_file)!r}).write_text(str(p.pid))")
    command = [
        sys.executable, str(watchdog),
        "--rss-mib", "256", "--seconds", "5",
        "--orphan-grace-ms", "100", "--log", str(log), "--",
        sys.executable, "-c", child_script,
    ]
    result = subprocess.run(command, capture_output=True, text=True, timeout=10)
    assert result.returncode != 0
    assert "[watchdog_reason] orphan" in log.read_text()
    child_pid = int(pid_file.read_text())
    for _ in range(30):
        try:
            os.kill(child_pid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.05)
    else:
        pytest.fail(f"captured orphan PID {child_pid} survived watchdog teardown")


def test_rss_watchdog_reaps_orphans_before_returning_to_outer_guard(tmp_path: Path):
    watchdog = ROOT / "scripts/test/sniper_rss_watch.py"
    inner_log = tmp_path / "inner.log"
    outer_log = tmp_path / "outer.log"
    pid_file = tmp_path / "orphan.pid"
    orphan = (
        "import pathlib,subprocess,sys;"
        "p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)']);"
        f"pathlib.Path({str(pid_file)!r}).write_text(str(p.pid))")
    inner = [
        sys.executable, str(watchdog), "--rss-mib", "128", "--seconds", "5",
        "--orphan-grace-ms", "100", "--log", str(inner_log), "--",
        sys.executable, "-c", orphan,
    ]
    parent = (
        "import pathlib,subprocess;"
        f"result=subprocess.run({inner!r});"
        "assert result.returncode != 0;"
        f"pid=pathlib.Path({str(pid_file)!r}).read_text();"
        "assert not pathlib.Path('/proc',pid).exists(), 'inner watchdog left an unreaped orphan'")
    result = subprocess.run([
        sys.executable, str(watchdog), "--rss-mib", "256", "--seconds", "10",
        "--log", str(outer_log), "--", sys.executable, "-c", parent,
    ], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr + outer_log.read_text()
    assert "[watchdog_reason] orphan" in inner_log.read_text()
    assert "[watchdog_reason] exit" in outer_log.read_text()


@pytest.mark.parametrize("reason,rss,seconds,allocation", [
    ("rss-limit", 16, 5, "data=bytearray(64*1024*1024);"),
    ("timeout", 256, 1, ""),
])
def test_rss_watchdog_enforces_positive_limits(tmp_path, reason, rss, seconds, allocation):
    log = tmp_path / "bounded.log"
    pid_file = tmp_path / "child.pid"
    child = (
        "import os,pathlib,time;"
        f"pathlib.Path({str(pid_file)!r}).write_text(str(os.getpid()));"
        + allocation + "time.sleep(10)")
    result = subprocess.run([
        sys.executable, str(ROOT / "scripts/test/sniper_rss_watch.py"),
        "--rss-mib", str(rss), "--seconds", str(seconds),
        "--log", str(log), "--", sys.executable, "-c", child,
    ], capture_output=True, text=True, timeout=15)
    assert result.returncode != 0
    assert f"[watchdog_reason] {reason}" in log.read_text()
    assert not Path("/proc", pid_file.read_text()).exists()


def test_foundation_probe_compiles(tmp_path: Path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ unavailable")
    binary = tmp_path / "foundation_probe"
    result = subprocess.run([
        compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
        "-fopenmp",
        "-I", str(ROOT / "bench/include"),
        "-I", str(ROOT / "bench/include/external/gapbs"),
        "-I", str(ROOT / "bench/include/sniper_sim/snipersim/include"),
        str(ROOT / "bench/src_sniper/foundation_probe.cc"),
        "-o", str(binary),
    ], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    native = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=10)
    assert native.returncode == 0, native.stdout + native.stderr
    assert "values=1 loaded_protocol=0 live_response=0" in native.stdout
    assert "echoed=0x0000000000000005" in native.stdout


def test_record_runtime_cpp(tmp_path: Path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ unavailable")
    binary = tmp_path / "test_record_runtime"
    result = subprocess.run([
        compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
        "-fopenmp",
        "-I", str(ROOT / "bench/include"),
        "-I", str(ROOT / "bench/include/external/gapbs"),
        "-I", str(ROOT / "bench/include/sniper_sim/snipersim/include"),
        str(ROOT / "bench/src_sniper/test_ecg_record_sniper_runtime.cc"),
        str(ROOT / (
            "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
            "cache/ecg_record_sniper_runtime.cc")),
        "-o", str(binary),
    ], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    executed = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=10)
    assert executed.returncode == 0, executed.stdout + executed.stderr
    assert "[SUMMARY] failures=0" in executed.stdout


def test_record_path_is_wired_without_legacy_oracle():
    guest = read("bench/src_sniper/ecg_record_guest.h")
    kernel = read("bench/src_sniper/sg_kernel.cc")
    runtime = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/ecg_record_sniper_runtime.cc")
    integration = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/ecg_record_sniper.cc")
    setup = read("scripts/setup_sniper.py")

    for knob in (
        "SNIPER_ECG_RECORD_MECHANISM",
        "SNIPER_ECG_RECORD_BYTES",
        "SNIPER_ECG_RECORD_MINIMUM_MANTISSA_BITS",
        "SNIPER_ECG_RECORD_UPDATE_LATENCY",
        "SNIPER_ECG_RECORD_CAPTURE_WIDTH",
        "SNIPER_ECG_RECORD_PREFETCH_QUEUE",
        "SNIPER_ECG_RECORD_PREFETCH_LATENCY",
        "SNIPER_ECG_RECORD_LINE_BYTES",
    ):
        assert (
            knob in guest or knob in runtime or
            knob in integration or knob in setup)
    assert "EcgRecordPrStream<Graph>" in kernel
    assert "ecg_record::buildRecords" in guest
    assert "ecg_record::selectWindowTarget" in runtime
    assert "configuration.control &" in runtime
    assert "ecg_record::kNativeManagedPasses" in runtime
    assert "certified" not in guest.lower()
    assert "future lookup" not in guest.lower()
    assert "serviceEcgRecord(bool drain)" in setup
    assert "notePrefetchPrivateLookup" in setup
    assert "notePrefetchCompletionAdmissionCheck" in setup
    for marker in (
            "PROPERTY_DESCRIPTOR", "kWorkPassClose", "kWorkInvalidate",
            "ordinary_invalidations", "invalidation_cycles"):
        assert marker in guest + runtime + integration
    assert "class EcgRecordContext" in guest
    for method in ("bind(", "beginPass(", "closePass(", "loadRecord(",
                   "loadProperty(", "drain(", "finish("):
        assert method in guest
    harness = read("bench/include/sniper_sim/sniper_harness.h")
    context = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/graph_cache_context_sniper.cc")
    assert '\\"stride\\": %u' in harness
    assert 'parseJsonUint(obj, "\\"stride\\"")' in context


def test_record_completion_and_drain_use_real_time():
    setup = read("scripts/setup_sniper.py")
    kernel = read("bench/src_sniper/sg_kernel.cc")
    runtime = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/ecg_record_sniper_runtime.cc")
    before = setup.index("processMemOpFromCore(")
    completion = setup.index(
        "const UInt64 ecg_record_completion_cycle", before)
    assert completion > before
    assert "advanceCycle" not in runtime
    integration = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/ecg_record_sniper.cc")
    normalize = integration.split(
        "normalizeCycle(uint32_t core_id", 1)[1].split(
        "\n}\n", 1)[0]
    assert "std::max" in normalize
    assert "+ 1" not in normalize
    current_issue = setup.split(
        "before_access_new =", 1)[1].split(
        "replace_once(", 1)[0]
    current_service = setup.split(
        "memory_service = r'''", 1)[1].split("'''", 1)[0]
    assert "advanceCycle" not in current_issue
    assert "advanceCycle" not in current_service
    assert "delta * period" in setup
    assert "drainLimitCycles" in setup
    record_path = kernel[kernel.index("if (record_stream.active())"):]
    assert record_path.index("record_stream.finish();") < (
        record_path.index("SNIPER_ROI_END();"))
    assert "front.ready_cycle > cycle" in runtime
    assert "bool drain" not in runtime.split(
        "Runtime::takeReadyPrefetch", 1)[1].split("{", 1)[0]


def test_record_prefetch_is_request_scoped_llc_only():
    setup = read("scripts/setup_sniper.py")
    assert "doEcgRecordPrefetch" in setup
    assert "m_ecg_record_bypass" in setup
    assert "request-scoped private-allocation bypasses" in setup
    record_service = setup.split(
        "MemoryManager::serviceEcgRecord", 1)[1].split("'''", 1)[0]
    assert "l2->doEcgRecordPrefetch(" in record_service
    assert "l2->doPrefetch(" not in record_service
    assert "hasEcgRecordPrivateCopy" in setup
    assert "notePrefetchDemandMerge" in setup
    assert "record_memory->hasEcgRecordPrivateCopy(address)" in setup
    assert "4 * lookup_cycles" in setup
    assert "noteLookupCycles" in setup
    assert "block_info->setCState(CacheState::MODIFIED)" in setup
    duplicate_branch = setup.split(
        "if (record_prefetch) {", 1)[1].split(
        "block_info->setCState(CacheState::MODIFIED)", 1)[0]
    assert "return boost::tuple" in duplicate_branch
    response_bypass = setup.split(
        "ECG record requests populate only the shared NUCA", 1)[1]
    assert "m_ecg_record_bypass.count(address)" in response_bypass
    assert "SharedCacheBlockInfo* prefetched" in response_bypass
    assert "Ordinary prefetch response did not allocate" in response_bypass
    method = setup.split("CacheCntlr::doEcgRecordPrefetch(", 1)[1].split(
        "/*****", 1)[0]
    assert method.count("releaseStackLock(prefetch_address);") == 2
    assert "if (hit_where != HitWhere::MISS)" not in method
    assert "An LLC-resident ECG target remains LLC-only" in setup
    assert "A synchronous NUCA response must not populate private L2" in setup
    assert "LLC-only ECG prefetches have no private data array to read" in setup
    assert "This transaction fetches into NUCA only" in setup
    assert "ECG LLC-only DRAM reply acquired private coherence state" in setup


def test_record_geometry_requires_real_modulo_for_24mib():
    integration = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/ecg_record_sniper.cc")
    cache_base = read(
        "bench/include/sniper_sim/snipersim/common/core/"
        "memory_subsystem/cache/cache_base.cc")
    sets = 24 * 1024 * 1024 // (16 * 64)
    assert sets == 24576 and sets & (sets - 1)
    assert "set_index = block_num % m_num_sets;" in cache_base
    assert 'nuca_hash != "mod"' in integration
    assert "non-power-of-two NUCA geometry" in integration
    assert "dead_miss_bypass=llc-request-scoped" in integration


def test_record_updates_do_not_rewrite_demand_rrpv():
    policy = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/cache_set_ecg.cc")
    body = policy.split(
        "CacheSetECG::applyRecordUpdate", 1)[1].split(
        "CacheSetECG::canAdmitRecordPrefetch", 1)[0]
    assert "m_rrip_bits" not in body
    victim = policy.split(
        "CacheSetECG::getReplacementIndex", 1)[1].split(
        "tryLoadContext();", 1)[0]
    assert "isValidReplacement(way)" in victim
    assert "eligible_count" in victim
    assert "m_record_property[way]" in victim
    assert "m_record_tier[way]" in victim
    assert "have_watermark" in victim
    assert "graphbrew::sniper::record::victimState(" in victim
    assert "if (graphbrew::sniper::record::receiverWatermark(" not in victim
    assert "setRecordClass(way, update.property_vaddr)" in body
    assert "globalContext().classifyGRASP" in policy
    assert "observeOrdinaryLine(" in policy
    assert "invalidateRecordMetadata()" in policy
    integration = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/ecg_record_sniper.cc")
    assert "core.configured && core.runtime.active()" in integration
    assert "memory_virtual_line" in integration
    assert "prefetch_virtual_line" in integration


def test_record_guest_scans_actual_ids_and_parses_unsigned_strictly():
    guest = read("bench/src_sniper/ecg_record_guest.h")
    assert "ecg_record_max_source_id" in guest
    assert "requirements_.max_vertex_id = ecg_record_max_source_id" in guest
    assert "requirements_.vertex_count - 1" not in guest
    assert "errno == ERANGE" in guest
    assert "*digit < '0' || *digit > '9'" in guest
    assert "currently requires 64-byte cache lines" in guest
    integration = read(
        "bench/include/sniper_sim/overlays/common/core/memory_subsystem/"
        "cache/ecg_record_sniper.cc")
    assert "currently requires one simulated core" in integration
    assert "core_scope=single-core" in integration


def test_sniper_guest_build_tracks_local_record_header():
    result = subprocess.run(
        ["make", "-n", "-W", "bench/src_sniper/ecg_record_guest.h", "sniper-sg_kernel"],
        cwd=ROOT, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "-o bench/bin_sniper/sg_kernel" in result.stdout, (
        "Changing the real record guest header must rebuild sg_kernel")


def test_record_setup_dry_run_normalizes_complete_functions():
    module = runpy.run_path(str(ROOT / "scripts/setup_sniper.py"))
    module["_DRY_RUN_OVERLAY_TEXT"].clear()
    module["patch_sniper_foundation"](SimpleNamespace(dry_run=True))
    virtual = module["_DRY_RUN_OVERLAY_TEXT"]
    sniper = ROOT / "bench/include/sniper_sim/snipersim"
    def installed(relative: str) -> str:
        path = (sniper / relative).resolve()
        return virtual.get(path, path.read_text())

    memory = installed(
        "common/core/memory_subsystem/"
        "parametric_dram_directory_msi/memory_manager.cc")
    cache_cntlr = installed(
        "common/core/memory_subsystem/"
        "parametric_dram_directory_msi/cache_cntlr.cc")
    nuca = installed(
        "common/core/memory_subsystem/"
        "parametric_dram_directory_msi/nuca_cache.cc")
    directory = installed(
        "common/core/memory_subsystem/"
        "pr_l1_pr_l2_dram_directory_msi/dram_directory_cntlr.cc")

    service = memory.split(
        "MemoryManager::serviceEcgRecord("
        "bool drain, UInt64 start_cycle)", 1)[1].split(
        "\n}\n", 1)[0]
    assert "advanceCycle" not in service
    assert "delta * period" in service
    assert "noteDrainCycles(core_id, delta)" in service
    assert "takeReadyPrefetch(\n\t\t\t\tcore_id, cycle, request)" in service
    assert service.index("const UInt64 request_issue_cycle") < service.index(
        "beginPrefetchIssue(")
    assert "request.sequence, request_issue_cycle" in service
    assert memory.count("MemoryManager::invalidateEcgRecordBinding()") == 1
    assert "invalidateEcgRecordMetadataSet(set)" in memory
    access = memory.split(
        "MemoryManager::coreInitiateMemoryAccess(", 1)[1]
    assert "ecg_record_access" in access
    assert access.index("processMemOpFromCore(") < access.index(
        "ecg_record_completion_cycle")
    assert "ecg_record_post_access_time - t_cache_issue" in access
    assert "ecg_record_issue_cycle, ecg_record_latency_cycles" in access
    assert "doEcgRecordPrefetch(" in cache_cntlr
    assert "m_ecg_record_bypass.count(address)" in cache_cntlr
    assert "notePrefetchDemandMerge" in cache_cntlr
    response = cache_cntlr.split(
        "SharedCacheBlockInfo* prefetched", 1)[1].split(
        "// Set the Counters", 1)[0]
    assert "if (prefetched)" in response
    assert "m_ecg_record_bypass.count(address)" in response
    assert "Ordinary prefetch response did not allocate" in response
    method = cache_cntlr.split(
        "CacheCntlr::doEcgRecordPrefetch(", 1)[1].split(
        "/*****", 1)[0]
    assert method.count("releaseStackLock(prefetch_address);") == 2
    assert "if (hit_where != HitWhere::MISS)" not in method
    assert "An LLC-resident ECG target remains LLC-only" in cache_cntlr
    assert "A synchronous NUCA response must not populate private L2" in cache_cntlr
    assert "LLC-only ECG prefetches have no private data array to read" in cache_cntlr
    assert "notePrefetchCompletionPrivateDuplicate" in nuca
    assert "notePrefetchCompletionResident" in nuca
    sh_request = directory.split(
        "DramDirectoryCntlr::processShReqFromL2Cache", 1)[1].split(
        "void\nDramDirectoryCntlr::retrieveDataAndSendToL2Cache", 1)[0]
    assert "This transaction fetches into NUCA only" in sh_request
    assert "directory_entry->getNumSharers() == 0" in sh_request
    dram_reply = directory.split(
        "DramDirectoryCntlr::processDRAMReply", 1)[1].split(
        "void\nDramDirectoryCntlr::processInvRepFromL2Cache", 1)[0]
    assert "ECG LLC-only DRAM reply acquired private coherence state" in dram_reply
    assert "reply_msg_type = ShmemMsg::SH_REP" in dram_reply
