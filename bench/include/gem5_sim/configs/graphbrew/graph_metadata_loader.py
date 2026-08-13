#!/usr/bin/env python3
"""
Graph metadata loader for gem5 simulation.

Loads GraphCacheContext metadata from a JSON sideband file produced by the
GraphBrew pipeline. The metadata includes:
  - Property region addresses and bucket boundaries
  - Degree distribution and topology
  - Rereference matrix path (for P-OPT/ECG)
  - Mask configuration (for ECG)

The JSON file is produced by the pipeline before gem5 simulation runs:
    results/gem5_metadata/{graph_name}/context.json

This loader is used by the gem5 Python config (graph_se.py) to initialize
the GraphCacheContext that is shared by all replacement policy and prefetcher
SimObjects.

Usage:
    ctx = load_graph_metadata("results/gem5_metadata/soc-pokec/context.json")
    # ctx is a dict with all metadata fields
"""

import json
import os
from pathlib import Path


def load_graph_metadata(json_path):
    """Load graph cache context metadata from JSON sideband file.

    Args:
        json_path: Path to the JSON metadata file.

    Returns:
        dict with metadata fields:
        {
            "property_regions": [
                {
                    "base_address": int,
                    "upper_bound": int,
                    "num_elements": int,
                    "elem_size": int,
                    "num_buckets": int,
                    "bucket_bounds": [int, ...]
                }, ...
            ],
            "topology": {
                "num_vertices": int,
                "num_edges": int,
                "avg_degree": float,
                "num_buckets": int,
                "bucket_vertex_counts": [int, ...]
            },
            "mask_config": {
                "mask_width": int,
                "dbg_bits": int,
                "popt_bits": int,
                "prefetch_bits": int,
                "num_buckets": int,
                "rrpv_max": int,
                "ecg_mode": str,
                "enabled": bool
            },
            "rereference": {
                "matrix_file": str,
                "num_epochs": int,
                "num_cache_lines": int,
                "epoch_size": int,
                "base_address": int,
                "enabled": bool
            },
            "edge_array": {
                "base_address": int,
                "size": int,
                "elem_size": int
            }
        }

    Raises:
        FileNotFoundError: If the JSON file does not exist.
        json.JSONDecodeError: If the JSON is malformed.
    """
    path = Path(json_path)
    if not path.exists():
        raise FileNotFoundError(
            f"Graph metadata file not found: {json_path}\n"
            f"Run the pipeline first to generate metadata:\n"
            "  run scripts/experiments/ecg/roi_matrix.py so the benchmark "
            "exports the required sideband metadata"
        )

    with open(path, "r") as f:
        metadata = json.load(f)

    # Validate required fields
    required_sections = ["property_regions", "topology"]
    for section in required_sections:
        if section not in metadata:
            raise ValueError(
                f"Missing required section '{section}' in {json_path}")

    return metadata


def create_empty_metadata():
    """Create a minimal metadata structure for testing without a real graph."""
    return {
        "property_regions": [],
        "topology": {
            "num_vertices": 0,
            "num_edges": 0,
            "avg_degree": 0.0,
            "num_buckets": 11,
            "bucket_vertex_counts": [],
        },
        "mask_config": {
            "mask_width": 8,
            "dbg_bits": 2,
            "popt_bits": 4,
            "prefetch_bits": 2,
            "num_buckets": 11,
            "rrpv_max": 7,
            "ecg_mode": "DBG_PRIMARY",
            "enabled": False,
        },
        "rereference": {
            "matrix_file": "",
            "num_epochs": 256,
            "num_cache_lines": 0,
            "epoch_size": 0,
            "base_address": 0,
            "enabled": False,
        },
        "edge_array": {
            "base_address": 0,
            "size": 0,
            "elem_size": 4,
        },
    }


def metadata_summary(metadata):
    """Print a human-readable summary of the loaded metadata."""
    topo = metadata.get("topology", {})
    mask = metadata.get("mask_config", {})
    reref = metadata.get("rereference", {})
    regions = metadata.get("property_regions", [])

    lines = [
        "Graph Metadata Summary:",
        f"  Vertices:    {topo.get('num_vertices', 0):,}",
        f"  Edges:       {topo.get('num_edges', 0):,}",
        f"  Avg degree:  {topo.get('avg_degree', 0):.2f}",
        f"  Buckets:     {topo.get('num_buckets', 11)}",
        f"  Regions:     {len(regions)}",
        f"  Mask:        {'enabled' if mask.get('enabled') else 'disabled'}"
                     f" ({mask.get('mask_width', 8)}-bit"
                     f" DBG={mask.get('dbg_bits', 2)}"
                     f" POPT={mask.get('popt_bits', 4)}"
                     f" PFX={mask.get('prefetch_bits', 2)})",
        f"  ECG mode:    {mask.get('ecg_mode', 'DBG_PRIMARY')}",
        f"  Rereference: {'enabled' if reref.get('enabled') else 'disabled'}"
                     f" ({reref.get('num_epochs', 0)} epochs"
                     f" × {reref.get('num_cache_lines', 0)} lines)",
    ]
    return "\n".join(lines)
