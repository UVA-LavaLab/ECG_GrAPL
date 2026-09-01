import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
PATH = ROOT / "scripts/experiments/ecg/roi_matrix.py"
SPEC = importlib.util.spec_from_file_location(
    "roi_matrix_explicit_env_hash_test", PATH)
ROI = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = ROI
SPEC.loader.exec_module(ROI)


def test_explicit_cell_environment_changes_matrix_hash(monkeypatch, tmp_path):
    args = SimpleNamespace(
        suite="cache-sim",
        benchmark="pr",
        options="-g 10 -k 16 -o 5 -n 1 -i 1",
        out_dir=tmp_path,
        dry_run=False,
    )
    policies = [SimpleNamespace(label="ECG_TEST")]
    monkeypatch.setenv(
        "GRAPHBREW_EXPLICIT_CELL_ENV",
        '{"CACHE_ECG_ADMISSION_SET_OFFSET":"0"}')
    offset_zero = ROI.standalone_matrix_config_hash(args, policies)
    monkeypatch.setenv(
        "GRAPHBREW_EXPLICIT_CELL_ENV",
        '{"CACHE_ECG_ADMISSION_SET_OFFSET":"1"}')
    offset_one = ROI.standalone_matrix_config_hash(args, policies)
    assert offset_zero != offset_one


def test_tierless_record_survives_the_explicit_cell_environment(monkeypatch):
    """ECG_RECORD_TIER_BITS=0 must reach the guest, not be scrubbed or defaulted.

    The tierless width is the only reason an n18 graph with 128 epochs fits in
    a 32-bit record at all, and "0" is exactly the value most easily lost to a
    falsy check or an environment scrub. Losing it does not fail: it silently
    produces an 8-byte cell labelled as compact.
    """
    monkeypatch.setenv(
        "GRAPHBREW_EXPLICIT_CELL_ENV", '{"ECG_RECORD_TIER_BITS":0}')
    assert ROI.explicit_ecg_record_tier_bits() == 0
    assert ROI.explicit_ecg_record_tier_bits(2) == 0

    env = {"ECG_RECORD_TIER_BITS": "2"}
    ROI.scrub_cell_mechanism_env(env)
    ROI.apply_explicit_cell_mechanism_env(
        env, SimpleNamespace(policy="ECG"))
    assert env["ECG_RECORD_TIER_BITS"] == "0"
    assert ROI.env_ecg_record_tier_bits(env) == 0

    sniper_env = {}
    ROI.apply_sniper_transport_cell_env(sniper_env)
    assert sniper_env["ECG_RECORD_TIER_BITS"] == "0"


def test_default_tier_width_is_unchanged_when_unspecified(monkeypatch):
    monkeypatch.setenv("GRAPHBREW_EXPLICIT_CELL_ENV", "{}")
    assert ROI.explicit_ecg_record_tier_bits() == 2
    assert ROI.env_ecg_record_tier_bits({}) == 2
    assert ROI.env_ecg_record_tier_bits({"ECG_RECORD_TIER_BITS": ""}) == 2


def test_undefined_tier_widths_fail_closed(monkeypatch):
    for raw in ("1", "3", "8", "two"):
        monkeypatch.setenv(
            "GRAPHBREW_EXPLICIT_CELL_ENV",
            '{"ECG_RECORD_TIER_BITS":"%s"}' % raw)
        try:
            ROI.explicit_ecg_record_tier_bits()
        except RuntimeError:
            continue
        raise AssertionError(
            f"ECG_RECORD_TIER_BITS={raw!r} was accepted, but the compact "
            "record layout defines only 0 and 2")


def test_sidecar_identity_includes_the_tier_width(monkeypatch, tmp_path):
    """One cache key per record layout, or a run reuses the wrong bytes."""
    monkeypatch.setenv("GRAPHBREW_EXPLICIT_CELL_ENV", "{}")
    args = SimpleNamespace(
        benchmark="pr",
        options="-g 10 -k 4 -o 5 -n 1 -i 1",
        ecg_epochs=128,
        expected_graph_sha256="",
        dry_run=True,
    )
    tiered = ROI.ensure_reuse_plan_sidecar(args, {}, 4, 2)
    tierless = ROI.ensure_reuse_plan_sidecar(args, {}, 4, 0)
    assert tiered != tierless, (
        "the sidecar cache key ignores the tier width, so a tiered sidecar "
        "would be handed to a tierless run")
