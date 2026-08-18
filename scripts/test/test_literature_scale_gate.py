import json
from pathlib import Path

from scripts.experiments.ecg.analysis import literature_scale_gate


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = json.loads(
    (ROOT / "scripts/experiments/ecg/experiment_manifest.json").read_text())
SCREEN = json.loads(
    (ROOT / "scripts/experiments/ecg/configs/"
     "pagerank_literature_scale.json").read_text())
CORPUS = ROOT / "results/graphs/literature_scale_corpus.receipt.json"


def test_literature_scale_gate_expected_shapes():
    screen = literature_scale_gate.expected_cells(
        MANIFEST, SCREEN, literature_scale_gate.SCREEN_STAGES)
    complete = literature_scale_gate.expected_cells(
        MANIFEST, SCREEN, literature_scale_gate.COMPLETE_STAGES)
    assert len(screen) == 13
    assert sum(len(roster) for roster in screen.values()) == 99
    assert len(complete) == 81
    assert sum(len(roster) for roster in complete.values()) == 447


def test_literature_scale_corpus_receipt_is_complete():
    if not CORPUS.is_file():
        return
    assert literature_scale_gate.validate_corpus(
        MANIFEST, CORPUS) == []


def test_empty_screen_gate_fails_closed():
    result = literature_scale_gate.evaluate(
        [], MANIFEST, SCREEN, [], "screen", CORPUS)
    assert result["valid"] is False
    assert result["cell_count"] == 0
    assert result["row_count"] == 0
    assert any(
        "missing literature-scale cells" in error
        for error in result["errors"])
    assert any(
        "PageRank timing rows incomplete" in error
        for error in result["errors"])
