import importlib.util
import json
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
PATH = ROOT / "scripts/experiments/ecg/flows/popt_artifact_repro.py"
SPEC = importlib.util.spec_from_file_location("popt_artifact_repro", PATH)
MOD = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules["popt_artifact_repro"] = MOD
SPEC.loader.exec_module(MOD)


def test_parser_requires_normal_completion_and_sums_llc_totals():
    text = """
~~~ PINTOOL STATS BEGIN ~~~
[LLC-STAT] Total Misses = 100
[LLC-STAT] Total Misses = 7
~~~ PINTOOL STATS END ~~~
[APP] Error = 0.25
[PIN-FINI] App Exit Code = 0
"""
    assert MOD.validate_completed_output(text) == (107, 0.25)
    with pytest.raises(ValueError):
        MOD.validate_completed_output(text.replace(
            "~~~ PINTOOL STATS END ~~~", ""))


def test_commands_pin_one_pagerank_sweep(tmp_path):
    root = tmp_path / "artifact"
    app_root = tmp_path / "apps"
    command = MOD.build_command(
        root, root / "pin", root / "tools", app_root,
        "uk-2002", "popt-8b", "/usr/bin/setarch")
    assert command[:3] == ["/usr/bin/setarch", "x86_64", "-R"]
    assert command[-6:] == [
        "-f", str(root / "input-graphs/uk-2002.sg"),
        "-n", "1", "-i", "1",
    ]
    assert str(app_root / "popt/pr") in command
    assert str(root / "tools/popt-8b/cache_pinsim.so") in command


def test_graph_provenance_must_match_actual_graph(tmp_path):
    root = tmp_path / "artifact"
    graphs = root / "input-graphs"
    graphs.mkdir(parents=True)
    graph = graphs / "g.sg"
    graph.write_bytes(b"graph")
    receipt = {
        "artifact_commit": MOD.PINNED_ARTIFACT_COMMIT,
        "graphs": {"g.sg": {"sha256": MOD.sha256(graph)}},
    }

    MOD.verify_graph_provenance(
        receipt, root, ["g"], MOD.PINNED_ARTIFACT_COMMIT)
    graph.write_bytes(b"changed")
    with pytest.raises(SystemExit):
        MOD.verify_graph_provenance(
            receipt, root, ["g"], MOD.PINNED_ARTIFACT_COMMIT)


def test_port_manifest_binds_script_pin_binary_app_and_sources(tmp_path):
    port = tmp_path / "port"
    pin_root = tmp_path / "pin"
    tool_root = port / "bin"
    app_root = port / "apps"
    script = tmp_path / "setup.py"
    script.write_text("setup\n")
    (pin_root / "intel64").mkdir(parents=True)
    (pin_root / "source/tools/Config").mkdir(parents=True)
    (pin_root / "pin").write_text("pin\n")
    (pin_root / "intel64/runtime").write_text("runtime\n")
    (pin_root / "source/tools/Config/rules").write_text("rules\n")
    policies = ["lru", "drrip", "popt-8b", "opt-ideal"]
    app_versions = {
        "lru": "baseline",
        "drrip": "baseline",
        "popt-8b": "popt",
        "opt-ideal": "opt-ideal",
    }
    binaries = {}
    source_trees = {}
    applications = {}
    app_source_trees = {}
    for policy in policies:
        (tool_root / policy).mkdir(parents=True)
        tool = tool_root / policy / "cache_pinsim.so"
        tool.write_text(f"{policy} tool\n")
        binaries[policy] = MOD.sha256(tool)
        source = port / "src" / policy
        source.mkdir(parents=True)
        (source / "source.cpp").write_text(f"{policy} source\n")
        source_trees[policy] = MOD.hash_tree(source)
    for app_version in set(app_versions.values()):
        (app_root / app_version).mkdir(parents=True)
        app = app_root / app_version / "pr"
        app.write_text(f"{app_version} app\n")
        applications[app_version] = {"pr": MOD.sha256(app)}
        source = port / "app-src" / app_version
        source.mkdir(parents=True)
        (source / "pr.cc").write_text(f"{app_version} app source\n")
        app_source_trees[app_version] = MOD.hash_tree(source)
    (port / "smoke").mkdir()
    smoke_graph = port / "smoke/tiny.sg"
    smoke_graph.write_text("graph\n")
    smoke_rows = []
    smoke_outputs = {}
    for policy in policies:
        smoke_stdout = port / "smoke" / f"{policy}.stdout"
        smoke_stdout.write_text(
            "~~~ PINTOOL STATS BEGIN ~~~\n"
            "[LLC-STAT] Total Misses = 10\n"
            "~~~ PINTOOL STATS END ~~~\n"
            "[APP] Error = 0.5\n"
            "[PIN-FINI] App Exit Code = 0\n")
        smoke_stderr = port / "smoke" / f"{policy}.stderr"
        smoke_stderr.write_text("")
        smoke_rows.append({
            "policy": policy,
            "exit_code": 0,
            "llc_demand_misses": 10,
            "app_error": 0.5,
            "stdout_sha256": MOD.sha256(smoke_stdout),
            "stderr_sha256": MOD.sha256(smoke_stderr),
        })
        smoke_outputs[policy] = smoke_stdout
    compiler_component = tmp_path / "compiler-component"
    compiler_component.write_text("compiler\n")
    manifest = {
        "schema_version": 2,
        "setup_script_sha256": MOD.sha256(script),
        "popt_repository": {"commit": MOD.PINNED_ARTIFACT_COMMIT},
        "policies": policies,
        "pin": {
            "pin_sha256": MOD.sha256(pin_root / "pin"),
            "intel64_tree_sha256": MOD.hash_tree(pin_root / "intel64"),
            "config_tree_sha256": MOD.hash_tree(
                pin_root / "source/tools/Config"),
        },
        "smoke": {
            "passed": True,
            "normal_application_completion": True,
            "semantic_error_match": True,
            "graph_sha256": MOD.sha256(smoke_graph),
            "rows": smoke_rows,
        },
        "build_environment": {
            "pin_wrapper_gcc": str(compiler_component),
            "pin_backend_compiler": {
                "path": str(compiler_component),
                "driver_sha256": MOD.sha256(compiler_component),
                "cc1plus": {
                    "path": str(compiler_component),
                    "sha256": MOD.sha256(compiler_component),
                },
                "libgcc": {
                    "path": str(compiler_component),
                    "sha256": MOD.sha256(compiler_component),
                },
                "libstdcxx": {
                    "path": str(compiler_component),
                    "sha256": MOD.sha256(compiler_component),
                },
                "search_dirs_sha256": "search",
                "native_target_flags_sha256": "target",
            },
        },
        "binaries": binaries,
        "generated_source_trees": source_trees,
        "applications": applications,
        "application_source_trees": app_source_trees,
    }
    receipt = port / "port_build_manifest.json"
    receipt.write_text(json.dumps(manifest))

    MOD.verify_port_build_manifest(
        manifest, receipt, MOD.sha256(script), MOD.PINNED_ARTIFACT_COMMIT,
        pin_root, tool_root, app_root, ["lru"])
    empty_manifest = dict(manifest)
    empty_manifest["policies"] = []
    empty_manifest["smoke"] = dict(manifest["smoke"], rows=[])
    with pytest.raises(SystemExit):
        MOD.verify_port_build_manifest(
            empty_manifest, receipt, MOD.sha256(script),
            MOD.PINNED_ARTIFACT_COMMIT,
            pin_root, tool_root, app_root, ["lru"])
    smoke_outputs["lru"].write_text("forged smoke\n")
    with pytest.raises(SystemExit):
        MOD.verify_port_build_manifest(
            manifest, receipt, MOD.sha256(script),
            MOD.PINNED_ARTIFACT_COMMIT,
            pin_root, tool_root, app_root, ["lru"])
    smoke_outputs["lru"].write_text(
        "~~~ PINTOOL STATS BEGIN ~~~\n"
        "[LLC-STAT] Total Misses = 10\n"
        "~~~ PINTOOL STATS END ~~~\n"
        "[APP] Error = 0.5\n"
        "[PIN-FINI] App Exit Code = 0\n")
    (tool_root / "lru/cache_pinsim.so").write_text("different\n")
    with pytest.raises(SystemExit):
        MOD.verify_port_build_manifest(
            manifest, receipt, MOD.sha256(script),
            MOD.PINNED_ARTIFACT_COMMIT,
            pin_root, tool_root, app_root, ["lru"])


def test_noncanonical_roots_can_never_enter_exact_mode(tmp_path):
    root = (tmp_path / "artifact").resolve()
    assert MOD.is_canonical_exact_mode(
        root, (root / "pin-2.14").resolve(),
        (root / "simulators").resolve(),
        (root / "applications").resolve(), None, None)
    assert not MOD.is_canonical_exact_mode(
        root, (root / "pin-4.2").resolve(),
        (root / "simulators").resolve(),
        (root / "applications").resolve(), None, None)


def test_exploratory_subset_is_not_full_public_gate():
    assert MOD.is_full_gate_shape(
        True, MOD.DEFAULT_GRAPHS, MOD.PUBLIC_POLICIES)
    assert not MOD.is_full_gate_shape(
        True, ["uk-2002"], ["drrip", "popt-8b"])
    assert not MOD.is_full_gate_shape(
        True, list(MOD.DEFAULT_GRAPHS) + ["uk-2002"],
        MOD.PUBLIC_POLICIES)


def test_public_claim_requires_pinned_receipts_and_no_resume():
    assert MOD.require_trusted_public_claim(
        True, False, False, False,
        MOD.PINNED_PUBLIC_PORT_MANIFEST_SHA256,
        MOD.PINNED_PUBLIC_GRAPH_PROVENANCE_SHA256)
    assert not MOD.require_trusted_public_claim(
        False, True, True, True, "untrusted", "untrusted")
    with pytest.raises(SystemExit):
        MOD.require_trusted_public_claim(
            True, False, True, False,
            MOD.PINNED_PUBLIC_PORT_MANIFEST_SHA256,
            MOD.PINNED_PUBLIC_GRAPH_PROVENANCE_SHA256)
    with pytest.raises(SystemExit):
        MOD.require_trusted_public_claim(
            True, False, False, False,
            "untrusted", MOD.PINNED_PUBLIC_GRAPH_PROVENANCE_SHA256)
    with pytest.raises(SystemExit):
        MOD.require_trusted_public_claim(
            True, False, False, True,
            MOD.PINNED_PUBLIC_PORT_MANIFEST_SHA256,
            MOD.PINNED_PUBLIC_GRAPH_PROVENANCE_SHA256)


def test_resume_revalidates_hashed_logs_and_completion(tmp_path):
    stdout = tmp_path / "g__lru.stdout"
    stderr = tmp_path / "g__lru.stderr"
    stdout.write_text(
        "~~~ PINTOOL STATS BEGIN ~~~\n"
        "[LLC-STAT] Total Misses = 10\n"
        "~~~ PINTOOL STATS END ~~~\n"
        "[APP] Error = 0.5\n"
        "[PIN-FINI] App Exit Code = 0\n")
    stderr.write_text("")
    fingerprints = {("g", "lru"): "fingerprint"}
    row = {
        "graph": "g",
        "policy": "lru",
        "exit_code": "0",
        "llc_demand_misses": "10",
        "app_error": "0.5",
        "execution_fingerprint": "fingerprint",
        "stdout_sha256": MOD.sha256(stdout),
        "stderr_sha256": MOD.sha256(stderr),
        "status": "ok",
    }
    validated = MOD.validate_resumed_rows(
        [row], tmp_path, fingerprints, ["g"], ["lru"], True)
    assert validated[0]["normal_completion_verified"] is True
    stdout.write_text("forged\n")
    with pytest.raises(SystemExit):
        MOD.validate_resumed_rows(
            [row], tmp_path, fingerprints, ["g"], ["lru"], True)


def test_resume_rows_round_trip_integer_metrics(tmp_path):
    rows = [{
        "graph": "g", "policy": "lru", "exit_code": 0,
        "llc_demand_misses": 123, "app_error": 0.5, "status": "ok",
    }]
    path = tmp_path / "results.csv"
    MOD.write_csv(path, rows)
    text = path.read_text()
    assert "123" in text and "lru" in text


def test_grasp_mode_is_explicitly_nonclaimable_rules_proxy():
    assert MOD.PROXY_GATE == "dbg-grasp-rules-proxy"
    assert MOD.GRASP_PROXY == "grasp-rules-proxy"
    source = PATH.read_text()
    assert '"popt_vs_grasp_direction": False' in source
    assert '"popt_vs_grasp_figure12_exact": False' in source
    assert '"grasp_rules_proxy_direction": False' in source


def test_hash_tree_rejects_missing_port_source(tmp_path):
    with pytest.raises(FileNotFoundError):
        MOD.hash_tree(tmp_path / "missing")
