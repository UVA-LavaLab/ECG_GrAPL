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
