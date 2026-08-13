#!/usr/bin/env python3
"""Build and smoke-test a provenance-bound Pin-4 port of P-OPT.

The public cache/application sources are copied from clean pinned checkouts.
The only public-policy source changes are Pin-4 name compatibility and a
non-owning graph reference for P-OPT/T-OPT, which fixes the artifact's shallow
graph copy without terminating the application before its correctness receipt.

The optional GRASP-derived arm is deliberately named a rules proxy. It applies
the official 3-bit GRASP RRIP rules to the P-OPT application's registered
IRREGDATA/REGDATA regions; it is not the official GRASP PageRank workload
mapping and cannot reproduce the original P-OPT-vs-GRASP result.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


POPT_COMMIT = "53b5021846690d0f3445428c6380e877ecf7a10e"
GRASP_COMMIT = "6e3814430265fc4f2513c95ef131a6522bc9d389"
PUBLIC_POLICIES = ("lru", "drrip", "popt-8b", "opt-ideal")
GRASP_PROXY = "grasp-rules-proxy"
APPLICATION_TARGETS = {
    "baseline": ("pr", "randomizer"),
    "popt": ("pr",),
    "opt-ideal": ("pr",),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def hash_tree(path: Path) -> str:
    if not path.exists():
        raise FileNotFoundError(path)
    if path.is_file():
        return sha256(path)
    digest = hashlib.sha256()
    for child in sorted(item for item in path.rglob("*") if item.is_file()):
        digest.update(str(child.relative_to(path)).encode())
        digest.update(sha256(child).encode())
    return digest.hexdigest()


def command_output(command: list[str], cwd: Path | None = None) -> str:
    return subprocess.run(
        command, cwd=cwd, capture_output=True, text=True,
        check=True).stdout.strip()


def compiler_receipt(cxx: str) -> dict:
    def printed_file(*args: str) -> dict:
        path = Path(command_output([cxx, *args])).resolve()
        if not path.is_file():
            raise SystemExit(
                f"compiler component is not a file: {' '.join(args)} -> {path}")
        return {
            "path": str(path),
            "sha256": sha256(path),
        }

    target_flags = command_output(
        [cxx, "-march=native", "-Q", "--help=target"])
    return {
        "path": cxx,
        "driver_sha256": sha256(Path(cxx)),
        "version": command_output([cxx, "--version"]).splitlines()[0],
        "dumpmachine": command_output([cxx, "-dumpmachine"]),
        "dumpversion": command_output([cxx, "-dumpversion"]),
        "cc1plus": printed_file("-print-prog-name=cc1plus"),
        "libgcc": printed_file("-print-libgcc-file-name"),
        "libstdcxx": printed_file("-print-file-name=libstdc++.so"),
        "search_dirs_sha256": hashlib.sha256(
            command_output([cxx, "-print-search-dirs"]).encode()).hexdigest(),
        "native_target_flags_sha256": hashlib.sha256(
            target_flags.encode()).hexdigest(),
    }


def git_value(path: Path, *args: str) -> str:
    return command_output(["git", *args], cwd=path)


def ensure_clean_repo(path: Path, commit: str, label: str) -> dict:
    head = git_value(path, "rev-parse", "HEAD")
    if head != commit:
        raise SystemExit(f"unexpected {label} commit: {head} != {commit}")
    status = git_value(
        path, "status", "--porcelain", "--untracked-files=no")
    if status:
        raise SystemExit(f"{label} checkout has tracked modifications:\n{status}")
    remote = subprocess.run(
        ["git", "config", "--get", "remote.origin.url"], cwd=path,
        capture_output=True, text=True, check=False).stdout.strip()
    return {
        "commit": head,
        "tree": git_value(path, "rev-parse", "HEAD^{tree}"),
        "remote": remote,
        "tracked_clean": True,
    }


def copy_simulator_sources(source: Path, target: Path) -> None:
    target.mkdir(parents=True, exist_ok=True)
    copied = 0
    for item in source.iterdir():
        if item.is_file() and item.suffix in {".cpp", ".h"}:
            shutil.copy2(item, target / item.name)
            copied += 1
    if copied == 0 or not (target / "cache_pinsim.cpp").is_file():
        raise SystemExit(f"no simulator sources copied from {source}")


def copy_application_sources(source: Path, target: Path) -> None:
    target.mkdir(parents=True, exist_ok=True)
    for item in source.iterdir():
        if item.is_file() and (
                item.suffix in {".cc", ".h"} or item.name == "Makefile"):
            shutil.copy2(item, target / item.name)
    if not (target / "Makefile").is_file() or not (target / "pr.cc").is_file():
        raise SystemExit(f"incomplete application sources in {source}")


def patch_nonowning_graph(source: Path) -> None:
    header = source / "llc.h"
    header_text = header.read_text()
    needle = "        Graph m_graph;"
    replacement = "        Graph* m_graph {nullptr};"
    if header_text.count(needle) != 1:
        raise SystemExit(f"owned graph declaration not found once in {header}")
    header.write_text(header_text.replace(needle, replacement, 1))

    cpp = source / "llc.cpp"
    cpp_text = cpp.read_text()
    register = """void LLC::registerGraph(Graph &g, bool isPull)
{
    m_graph.setGraphProperties(g.num_nodes(), g.num_edges(), g.directed());
    m_graph.setGraphDatastructures(g.out_index(), g.out_neighbors(),
                                   g.in_index(), g.in_neighbors());
    m_isPull = isPull;

}"""
    if register not in cpp_text:
        register = """void LLC::registerGraph(Graph &g, bool isPull)
{
    m_graph.setGraphProperties(g.num_nodes(), g.num_edges(), g.directed());
    m_graph.setGraphDatastructures(g.out_index(), g.out_neighbors(),
                                   g.in_index(), g.in_neighbors());
    m_isPull = isPull;
}"""
    if cpp_text.count(register) != 1:
        raise SystemExit(f"graph registration block not found once in {cpp}")
    replacement = """void LLC::registerGraph(Graph &g, bool isPull)
{
    m_graph = &g;
    m_isPull = isPull;
}"""
    cpp_text = cpp_text.replace(register, replacement, 1)
    cpp_text = cpp_text.replace("m_graph.", "m_graph->")
    cpp.write_text(cpp_text)


def patch_fini_receipt(source: Path) -> None:
    cpp = source / "cache_pinsim.cpp"
    text = cpp.read_text()
    needle = """VOID Fini(INT32 code, VOID *v)
{
    std::cout << "[PINTOOL] No. of Instructions = " << numInsns << std::endl;
    cache.reportTotalStats();
}"""
    replacement = """VOID Fini(INT32 code, VOID *v)
{
    std::cout << "[PIN-FINI] App Exit Code = " << code << std::endl;
}"""
    if text.count(needle) != 1:
        raise SystemExit(f"Fini stats hook not found once in {cpp}")
    cpp.write_text(text.replace(needle, replacement, 1))


def parse_grasp_rules(grasp_root: Path) -> dict:
    grasp_cpp = grasp_root / "trace-based-simulators/grasp.cpp"
    common_h = grasp_root / "trace-based-simulators/common.h"
    text = grasp_cpp.read_text()
    bits_match = re.search(r"const int num_bits_rrip = (\d+);", text)
    priority_match = re.search(r"const int P_RRIP = (\d+);", text)
    hit_match = re.search(r"const int H_RRIP = (\d+);", text)
    if not bits_match or not priority_match or not hit_match:
        raise SystemExit("official GRASP RRIP constants are not recognizable")
    common = common_h.read_text()
    required = (
        "is_in_high_reuse_region",
        "is_in_moderate_reuse_region",
        "border_high_reuse = regions[i].min + (f)",
        "border_moderate_reuse = regions[i].min + (2*f)",
    )
    missing = [item for item in required if item not in common]
    if missing:
        raise SystemExit(
            "official GRASP region rules changed: " + ", ".join(missing))
    bits = int(bits_match.group(1))
    maximum = (1 << bits) - 1
    return {
        "rrip_bits": bits,
        "maximum_rrpv": maximum,
        "intermediate_insert_rrpv": maximum - 1,
        "priority_insert_rrpv": int(priority_match.group(1)),
        "priority_hit_rrpv": int(hit_match.group(1)),
        "grasp_cpp_sha256": sha256(grasp_cpp),
        "common_h_sha256": sha256(common_h),
    }


def make_grasp_proxy(
        source: Path, target: Path, grasp_root: Path) -> dict:
    rules = parse_grasp_rules(grasp_root)
    copy_simulator_sources(source, target)
    header = target / "llc.h"
    text = header.read_text().replace(
        "#include <cstdlib>", "#include <algorithm>\n#include <cstdlib>", 1)
    text = text.replace(
        """        void updateReplacementState(int setID, int wayID);
        void moveToMRU(int setID, int wayID);""",
        """        void updateReplacementState(int setID, int wayID);
        void setInsertionState(intptr_t addr, int setID, int wayID);
        bool isHighReuse(intptr_t addr);
        bool isModerateReuse(intptr_t addr);
        void moveToMRU(int setID, int wayID);""",
        1)
    header.write_text(text)

    cpp = target / "llc.cpp"
    text = cpp.read_text()
    init = "void LLC::Init()\n{"
    if text.count(init) != 1:
        raise SystemExit(f"LLC::Init not found once in {cpp}")
    text = text.replace(
        init,
        init + """
    std::cout << "[GRASP-RULES-PROXY] workload_mapping="
              << "popt-irregdata-regdata official_mapping=0" << std::endl;""",
        1)
    install = """    m_tagArray[setID][index] = addr; //new line inserted\x20
    //m_dirty[setID][index]    = (isWrite == true) ? 1 : 0;"""
    replacement = """    m_tagArray[setID][index] = addr; //new line inserted
    setInsertionState(addr, setID, index);
    //m_dirty[setID][index]    = (isWrite == true) ? 1 : 0;"""
    if text.count(install) != 1:
        raise SystemExit(f"GRASP proxy insertion site not found once in {cpp}")
    text = text.replace(install, replacement, 1)
    begin = text.index("int LLC::getReplacementIndex(")
    end = text.index("void LLC::moveToMRU(", begin)
    policy = f"""bool LLC::isHighReuse(intptr_t addr)
{{
    const uint64_t capacity =
        static_cast<uint64_t>(m_numSets) * m_numWays * m_lineSz;
    const uint64_t high_bytes = capacity / 2;
    for (int dTypeID : {{IRREGDATA, REGDATA}}) {{
        for (size_t i = 0; i < m_dType_addrStart[dTypeID].size(); ++i) {{
            const intptr_t start = m_dType_addrStart[dTypeID][i];
            const intptr_t end = m_dType_addrEnd[dTypeID][i];
            const intptr_t high =
                std::min<intptr_t>(end, start + high_bytes) + 8;
            if (addr >= start && addr < high) return true;
        }}
    }}
    return false;
}}

bool LLC::isModerateReuse(intptr_t addr)
{{
    const uint64_t capacity =
        static_cast<uint64_t>(m_numSets) * m_numWays * m_lineSz;
    const uint64_t high_bytes = capacity / 2;
    for (int dTypeID : {{IRREGDATA, REGDATA}}) {{
        for (size_t i = 0; i < m_dType_addrStart[dTypeID].size(); ++i) {{
            const intptr_t start = m_dType_addrStart[dTypeID][i];
            const intptr_t end = m_dType_addrEnd[dTypeID][i];
            const intptr_t high =
                std::min<intptr_t>(end, start + high_bytes) + 8;
            const intptr_t moderate =
                std::min<intptr_t>(end, start + 2 * high_bytes) + 8;
            if (addr >= high && addr < moderate) return true;
        }}
    }}
    return false;
}}

int LLC::getReplacementIndex(int setID, int setType, int tid)
{{
    (void)setType; (void)tid;
    for (int way = 0; way < m_numWays; ++way)
        if (m_tagArray[setID][way] == -1) return way;
    int victim = 0;
    int max_rrpv = m_rrpv[setID][0];
    for (int way = 1; way < m_numWays; ++way) {{
        if (m_rrpv[setID][way] > max_rrpv) {{
            max_rrpv = m_rrpv[setID][way];
            victim = way;
        }}
    }}
    if (max_rrpv < m_MAX_rrpv) {{
        const int diff = m_MAX_rrpv - max_rrpv;
        for (int way = 0; way < m_numWays; ++way)
            m_rrpv[setID][way] += diff;
    }}
    return victim;
}}

void LLC::setInsertionState(intptr_t addr, int setID, int wayID)
{{
    if (isHighReuse(addr))
        m_rrpv[setID][wayID] = {rules["priority_insert_rrpv"]};
    else if (isModerateReuse(addr))
        m_rrpv[setID][wayID] = {rules["intermediate_insert_rrpv"]};
    else
        m_rrpv[setID][wayID] = {rules["maximum_rrpv"]};
}}

void LLC::updateReplacementState(int setID, int wayID)
{{
    if (isHighReuse(m_tagArray[setID][wayID]))
        m_rrpv[setID][wayID] = {rules["priority_hit_rrpv"]};
    else if (m_rrpv[setID][wayID] > 0)
        --m_rrpv[setID][wayID];
}}

"""
    cpp.write_text(text[:begin] + policy + text[end:])
    return rules


def write_build_files(root: Path, pin_root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "compat.hpp").write_text(
        '#include "pin.H"\n#include <iostream>\n#include <string>\n'
        "using std::cerr;\nusing std::endl;\nusing std::string;\n")
    (root / "Makefile").write_text(f"""PIN_ROOT ?= {pin_root}
SRC_DIR ?= ../../src/lru
CONFIG_ROOT := $(PIN_ROOT)/source/tools/Config
include $(CONFIG_ROOT)/makefile.config
TOOL_ROOTS := cache_pinsim
OBJECT_ROOTS := cache_backend l1 l2 llc
VPATH := $(SRC_DIR)
TOOL_CXXFLAGS += -std=c++11 -DBIGARRAY_MULTIPLIER=1 -I$(SRC_DIR) \\
\t-include $(CURDIR)/compat.hpp
include $(TOOLS_ROOT)/Config/makefile.default.rules
$(OBJDIR)cache_pinsim$(PINTOOL_SUFFIX): \\
\t$(OBJDIR)cache_pinsim$(OBJ_SUFFIX) $(OBJECTS)
\t$(LINKER) $(TOOL_LDFLAGS) $(LINK_EXE)$@ $^ \\
\t\t$(TOOL_LPATHS) $(TOOL_LIBS)
""")


def parse_total_llc_misses(text: str) -> int:
    values = [
        int(float(line.rsplit(" ", 1)[-1]))
        for line in text.splitlines()
        if "[LLC-STAT] Total Misses" in line
    ]
    if not values:
        raise ValueError("smoke output contains no LLC Total Misses")
    return sum(values)


def parse_app_error(text: str) -> float:
    values = [
        float(line.split("=", 1)[1].strip())
        for line in text.splitlines()
        if line.startswith("[APP] Error =")
    ]
    if len(values) != 1 or not math.isfinite(values[0]):
        raise ValueError("smoke output lacks one finite [APP] Error receipt")
    return values[0]


def validate_completed_output(text: str) -> tuple[int, float]:
    markers = (
        "~~~ PINTOOL STATS BEGIN ~~~",
        "~~~ PINTOOL STATS END ~~~",
        "[APP] Error =",
        "[PIN-FINI] App Exit Code = 0",
    )
    if any(marker not in text for marker in markers):
        raise ValueError("smoke output lacks normal-completion markers")
    if not (
            text.index(markers[0]) < text.index(markers[1]) <
            text.index(markers[2]) < text.index(markers[3])):
        raise ValueError("smoke completion markers are out of order")
    return parse_total_llc_misses(text), parse_app_error(text)


def build_applications(artifact: Path, out: Path, cxx: str) -> tuple[dict, dict]:
    app_hashes = {}
    source_hashes = {}
    for name, targets in APPLICATION_TARGETS.items():
        source = out / "app-src" / name
        copy_application_sources(artifact / "applications" / name, source)
        subprocess.run(
            ["make", f"CXX={cxx}", "clean"], cwd=source, check=True,
            stdout=subprocess.DEVNULL)
        subprocess.run(
            ["make", f"CXX={cxx}", "-j4", *targets], cwd=source, check=True)
        app_hashes[name] = {}
        for target_name in targets:
            destination = out / "apps" / name / target_name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source / target_name, destination)
            app_hashes[name][target_name] = sha256(destination)
        subprocess.run(
            ["make", f"CXX={cxx}", "clean"], cwd=source, check=True,
            stdout=subprocess.DEVNULL)
        source_hashes[name] = hash_tree(source)
    return app_hashes, source_hashes


def smoke_test(
        out: Path, pin_root: Path, policies: list[str]) -> dict:
    setarch = shutil.which("setarch")
    if not setarch:
        raise SystemExit("setarch is required for deterministic smoke tests")
    smoke = out / "smoke"
    smoke.mkdir(parents=True, exist_ok=True)
    graph = smoke / "tiny.sg"
    env = dict(os.environ, OMP_NUM_THREADS="1")
    subprocess.run(
        [str(out / "apps/baseline/randomizer"),
         "-g", "8", "-k", "2", "-b", str(graph)],
        env=env, check=True, stdout=subprocess.DEVNULL)
    app_for_policy = {
        "lru": "baseline",
        "drrip": "baseline",
        "popt-8b": "popt",
        "opt-ideal": "opt-ideal",
        GRASP_PROXY: "baseline",
    }
    rows = []
    for policy in policies:
        command = [
            setarch, "x86_64", "-R",
            str(pin_root / "pin"),
            "-t", str(out / "bin" / policy / "cache_pinsim.so"),
            "--", str(out / "apps" / app_for_policy[policy] / "pr"),
            "-f", str(graph), "-n", "1", "-i", "1",
        ]
        result = subprocess.run(
            command, env=env, capture_output=True, text=True,
            timeout=300, check=False)
        stdout = smoke / f"{policy}.stdout"
        stderr = smoke / f"{policy}.stderr"
        stdout.write_text(result.stdout)
        stderr.write_text(result.stderr)
        if result.returncode != 0:
            raise SystemExit(
                f"{policy} smoke failed with exit {result.returncode}")
        misses, app_error = validate_completed_output(result.stdout)
        rows.append({
            "policy": policy,
            "exit_code": result.returncode,
            "llc_demand_misses": misses,
            "app_error": app_error,
            "stdout_sha256": sha256(stdout),
            "stderr_sha256": sha256(stderr),
        })
    reference = rows[0]["app_error"]
    if any(not math.isclose(
            row["app_error"], reference, rel_tol=1e-9, abs_tol=1e-12)
            for row in rows[1:]):
        raise SystemExit("smoke PageRank error differs across policy arms")
    return {
        "passed": True,
        "graph_sha256": sha256(graph),
        "normal_application_completion": True,
        "semantic_error_match": True,
        "rows": rows,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--pin-root", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--grasp-source-root", type=Path)
    parser.add_argument("--cxx", default=shutil.which("g++") or "g++")
    parser.add_argument(
        "--pin-wrapper-gcc",
        default=shutil.which(os.environ.get("PIN_WRAPPER_GCC", "gcc"))
        or "gcc")
    args = parser.parse_args(argv)
    artifact = args.artifact_root.resolve()
    pin_root = args.pin_root.resolve()
    out = args.out_dir.resolve()
    cxx = str(Path(args.cxx).resolve())
    pin_wrapper_gcc = str(Path(args.pin_wrapper_gcc).resolve())
    if not Path(pin_wrapper_gcc).is_file():
        raise SystemExit(f"Pin backend compiler not found: {pin_wrapper_gcc}")
    pin_build_env = dict(os.environ, PIN_WRAPPER_GCC=pin_wrapper_gcc)
    popt_repo = ensure_clean_repo(artifact, POPT_COMMIT, "P-OPT")
    grasp = args.grasp_source_root.resolve() if args.grasp_source_root else None
    grasp_repo = (
        ensure_clean_repo(grasp, GRASP_COMMIT, "GRASP") if grasp else {})
    if out.exists() and any(out.iterdir()):
        raise SystemExit("--out-dir must be absent or empty")
    out.mkdir(parents=True, exist_ok=True)
    if not (pin_root / "pin").is_file():
        raise SystemExit(f"Pin executable not found under {pin_root}")

    applications, app_source_hashes = build_applications(
        artifact, out, cxx)
    policies = list(PUBLIC_POLICIES)
    source_hashes = {}
    for policy in policies:
        target = out / "src" / policy
        copy_simulator_sources(artifact / "simulators" / policy, target)
        if policy in {"popt-8b", "opt-ideal"}:
            patch_nonowning_graph(target)
        patch_fini_receipt(target)
        source_hashes[policy] = hash_tree(target)

    grasp_proxy = {}
    if grasp:
        provenance = out / "provenance" / "grasp"
        provenance.mkdir(parents=True, exist_ok=True)
        for name in ("grasp.cpp", "common.h"):
            shutil.copy2(
                grasp / "trace-based-simulators" / name,
                provenance / name)
        grasp_proxy = make_grasp_proxy(
            artifact / "simulators" / "drrip",
            out / "src" / GRASP_PROXY, grasp)
        patch_fini_receipt(out / "src" / GRASP_PROXY)
        grasp_proxy.update({
            "workload_mapping": "popt-irregdata-regdata",
            "official_workload_mapping": False,
            "claimable_as_official_grasp": False,
            "protected_regions": ["IRREGDATA", "REGDATA"],
            "preserved_source_sha256": hash_tree(provenance),
        })
        source_hashes[GRASP_PROXY] = hash_tree(out / "src" / GRASP_PROXY)
        policies.append(GRASP_PROXY)

    binaries = {}
    for policy in policies:
        build = out / "build" / policy
        write_build_files(build, pin_root)
        subprocess.run(
            ["make", f"PIN_ROOT={pin_root}", "clean"],
            cwd=build, env=pin_build_env,
            stdout=subprocess.DEVNULL, check=True)
        subprocess.run(
            ["make", f"PIN_ROOT={pin_root}",
             f"SRC_DIR={out / 'src' / policy}", "-j4"],
            cwd=build, env=pin_build_env, check=True)
        target = out / "bin" / policy / "cache_pinsim.so"
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(build / "obj-intel64/cache_pinsim.so", target)
        binaries[policy] = sha256(target)

    smoke = smoke_test(out, pin_root, policies)
    cpu_model = ""
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith("model name"):
                cpu_model = line.split(":", 1)[1].strip()
                break
    application_compiler = compiler_receipt(cxx)
    manifest = {
        "schema_version": 2,
        "setup_script_sha256": sha256(Path(__file__).resolve()),
        "popt_repository": popt_repo,
        "grasp_repository": grasp_repo,
        "pin": {
            "version": command_output([str(pin_root / "pin"), "-version"])
            .splitlines()[0],
            "pin_sha256": sha256(pin_root / "pin"),
            "intel64_tree_sha256": hash_tree(pin_root / "intel64"),
            "config_tree_sha256": hash_tree(
                pin_root / "source/tools/Config"),
        },
        "build_environment": {
            "application_compiler": application_compiler,
            "artifact_documented_application_compiler": "g++-6.3.0",
            "application_compiler_is_compatibility_deviation": (
                "6.3.0" not in application_compiler["version"]),
            "pin_cxx": str(pin_root / "intel64/pinrt/bin/pin-g++"),
            "pin_cxx_sha256": sha256(
                pin_root / "intel64/pinrt/bin/pin-g++"),
            "pin_wrapper_gcc": pin_wrapper_gcc,
            "pin_backend_compiler": compiler_receipt(pin_wrapper_gcc),
            "make_version": command_output(["make", "--version"])
            .splitlines()[0],
            "machine": os.uname().machine,
            "cpu_model": cpu_model,
            "application_flags": "-std=c++11 -O3 -g -Wall "
                                 "-fopenmp -march=native",
            "pintool_flags": "-std=c++11 -DBIGARRAY_MULTIPLIER=1",
        },
        "policies": policies,
        "binaries": binaries,
        "applications": applications,
        "generated_source_trees": source_hashes,
        "application_source_trees": app_source_hashes,
        "compatibility_changes": [
            "restore old Pin global C++ names",
            "make P-OPT/T-OPT graph references non-owning",
            "replace duplicate Fini stats with an app-exit-code receipt",
        ],
        "grasp_rules_proxy": grasp_proxy,
        "smoke": smoke,
    }
    (out / "port_build_manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
