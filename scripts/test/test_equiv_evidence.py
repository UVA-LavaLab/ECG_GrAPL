import csv
import hashlib
import json
from pathlib import Path

from scripts.experiments.ecg.verify.equiv_kernels import (
    archive_cell,
    validate_roi_output,
)


def write_roi_output(root: Path) -> None:
    csv_path = root / "roi_matrix.csv"
    json_path = root / "roi_matrix.json"
    rows = [{"status": "ok", "policy_label": "ECG_K2"}]
    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    json_path.write_text(json.dumps(rows))
    marker = {
        "complete": True,
        "all_rows_ok": True,
        "outputs": {
            "roi_matrix.csv": {
                "rows": 1,
                "sha256": hashlib.sha256(csv_path.read_bytes()).hexdigest(),
            },
            "roi_matrix.json": {
                "rows": 1,
                "sha256": hashlib.sha256(json_path.read_bytes()).hexdigest(),
            },
        },
    }
    (root / "roi_matrix.complete.json").write_text(json.dumps(marker))


def test_roi_completion_and_cell_archive(tmp_path: Path):
    output = tmp_path / "output"
    output.mkdir()
    write_roi_output(output)
    errors, row = validate_roi_output(output, "ECG_K2")
    assert errors == []
    assert row["status"] == "ok"

    evidence = tmp_path / "evidence"
    record = archive_cell(
        evidence, "sniper", "pr", "raw trace", "[ECG-CONFIG]",
        {"victims": 1}, "ok", "ECG:epoch_first",
        {"output_dir": str(output), "errors": []})
    assert record["outputs"]["raw_log"]["sha256"]
    assert (
        evidence / "cells/pr/sniper/roi_matrix.complete.json").exists()

    marker = json.loads((output / "roi_matrix.complete.json").read_text())
    marker["all_rows_ok"] = False
    (output / "roi_matrix.complete.json").write_text(json.dumps(marker))
    errors, _ = validate_roi_output(output, "ECG_K2")
    assert "completion marker all_rows_ok is not true" in errors
