#!/usr/bin/env python3
"""Tier B: GraphBrew P-OPT rereference matrix matches the upstream algorithm.

The upstream P-OPT artifact from Balaji et al. compresses each
(cache_line, epoch) pair
into a single byte whose MSB selects between two encodings:

* MSB=0: cache line IS referenced in this epoch; the lower 7 bits hold the
  final sub-epoch where it was last referenced.
* MSB=1: cache line is NOT referenced in this epoch; the lower 7 bits hold
  the forward distance (in epochs) to the next referenced epoch, capped at
  127.

GraphBrew's ``makeOffsetMatrix`` (``bench/include/graphbrew/partition/cagra/
popt.h``) implements the same encoding after the MSB-polarity fix in commit
``c1372e1`` (``"if ((next_entry & OR_MASK) == 0) return 1"``).  This test
locks in the runtime equivalence by:

1. Compiling a tiny self-contained C++ harness that constructs a small CSR
   graph and calls ``makeOffsetMatrix`` from ``popt.h``.
2. Independently re-implementing the algorithm in Python (a strict transliteration
   of the upstream specification, kept inline below for auditability).
3. Diffing the two compressed matrices for **bit-exact** equality across two
   tiny synthetic graphs.

The evaluation methodology calls for
diffing against an upstream reference run on ``web-Google.el``.  The upstream
artifact source is not vendored in this repository (network clone required),
so this test uses an in-repo Python clone of the published algorithm as the
"reference" — combined with the existing source-level guard
(``scripts/test/test_popt_grasp_faithfulness_sources.py
::test_popt_rereference_encoding_matches_official_artifact_polarity``) which
already locks the MSB-polarity convention, this provides defence-in-depth
against silent drift in either implementation.
"""

from __future__ import annotations

import shutil
import os
import random
import struct
import subprocess
from pathlib import Path
from typing import Sequence

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[2]


# ---------------------------------------------------------------------------
# Python reference clone of makeOffsetMatrix() — read the C++ source side by
# side when modifying.
# ---------------------------------------------------------------------------

MAX_REREF = 127
OR_MASK = 0x80
AND_MASK = 0x7F


def make_offset_matrix_reference(
    num_nodes: int,
    adj_out: Sequence[Sequence[int]],
    num_vtx_per_line: int,
    num_epochs: int = 256,
) -> bytes:
    """Independent Python clone of ``makeOffsetMatrix`` for parity testing.

    Mirrors the three-step pipeline in
    ``bench/include/graphbrew/partition/cagra/popt.h::makeOffsetMatrix``.  The
    caller supplies a non-symmetrised out-adjacency (the C++ harness builds
    its CSR the same way to keep the comparison apples-to-apples).
    """

    if num_epochs != 256:
        raise ValueError("matrix encoder assumes 256 epochs")

    num_cache_lines = (num_nodes + num_vtx_per_line - 1) // num_vtx_per_line
    epoch_sz = (num_nodes + num_epochs - 1) // num_epochs
    sub_epoch_sz = (epoch_sz + 127) // 128

    # Step I: per (cache_line, epoch), record the largest referenced ngh ID.
    last_ref = [[-1] * num_epochs for _ in range(num_cache_lines)]
    for c in range(num_cache_lines):
        start = c * num_vtx_per_line
        end = num_nodes if c == num_cache_lines - 1 else (c + 1) * num_vtx_per_line
        for v in range(start, end):
            for ngh in adj_out[v]:
                e = ngh // epoch_sz
                if ngh > last_ref[c][e]:
                    last_ref[c][e] = ngh

    # Step II: compress to one byte.  Referenced epochs encode the sub-epoch
    # of the *last* reference; unreferenced epochs receive a sentinel that
    # Step II-b will overwrite with the forward distance.
    compressed = [[0] * num_epochs for _ in range(num_cache_lines)]
    for c in range(num_cache_lines):
        for e in range(num_epochs):
            v = last_ref[c][e]
            if v != -1:
                sub_pos = (v % epoch_sz) // sub_epoch_sz
                compressed[c][e] = sub_pos & AND_MASK
            else:
                compressed[c][e] = MAX_REREF | OR_MASK  # 0xFF sentinel

    # Step II-b: backward scan, overwriting MSB=1 entries with the forward
    # distance (capped at MAX_REREF).
    for c in range(num_cache_lines):
        dist = MAX_REREF
        for e in range(num_epochs - 1, -1, -1):
            if (compressed[c][e] & OR_MASK) == 0:
                dist = 1
            else:
                d = dist if dist < MAX_REREF else MAX_REREF
                compressed[c][e] = (d | OR_MASK) & 0xFF
                if dist < MAX_REREF:
                    dist += 1

    # Step III: transpose to epoch-major for cache-friendly access at runtime.
    out = bytearray(num_cache_lines * num_epochs)
    for c in range(num_cache_lines):
        for e in range(num_epochs):
            out[e * num_cache_lines + c] = compressed[c][e]
    return bytes(out)


# ---------------------------------------------------------------------------
# C++ harness: built once per pytest invocation, dumps matrix to argv[1].
# ---------------------------------------------------------------------------

POPT_DUMP_CC = r'''
// Test harness for test_popt_permutation_equivalence.py.
// Builds a tiny CSRGraph from a hard-coded edge list and dumps the
// makeOffsetMatrix() output to argv[1] as raw bytes.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <utility>
#include <vector>

#include <graph.h>
#include <pvector.h>
#include <timer.h>

#include "graphbrew/partition/cagra/popt.h"

using NodeID = int;

static void buildCSR(int N, const std::vector<std::pair<int,int>>& edges,
                     NodeID**& out_index, NodeID*& out_neigh) {
    std::vector<int> degree(N, 0);
    for (const auto& e : edges) degree[e.first]++;
    std::vector<int> offset(N + 1, 0);
    for (int v = 0; v < N; ++v) offset[v + 1] = offset[v] + degree[v];
    int total = offset[N];
    out_neigh = new NodeID[total];
    std::vector<int> cursor(N, 0);
    for (const auto& e : edges) {
        out_neigh[offset[e.first] + cursor[e.first]++] = e.second;
    }
    for (int v = 0; v < N; ++v) {
        std::sort(out_neigh + offset[v], out_neigh + offset[v + 1]);
    }
    out_index = new NodeID*[N + 1];
    for (int v = 0; v <= N; ++v) out_index[v] = out_neigh + offset[v];
}

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr,
                     "usage: %s OUT vtx_per_line epochs N E s0 d0 ...\n",
                     argv[0]);
        return 2;
    }
    const char* out_path = argv[1];
    int numVtxPerLine = std::atoi(argv[2]);
    int numEpochs = std::atoi(argv[3]);
    int N = std::atoi(argv[4]);
    int E = std::atoi(argv[5]);
    const int base = 6;
    if (argc < base + 2 * E) {
        std::fprintf(stderr, "not enough edge tokens\n");
        return 2;
    }
    std::vector<std::pair<int,int>> edges;
    edges.reserve(E);
    for (int i = 0; i < E; ++i) {
        int s = std::atoi(argv[base + 2 * i + 0]);
        int d = std::atoi(argv[base + 2 * i + 1]);
        edges.emplace_back(s, d);
    }
    NodeID** out_index = nullptr;
    NodeID* out_neigh = nullptr;
    buildCSR(N, edges, out_index, out_neigh);
    CSRGraph<NodeID, NodeID, true> g(static_cast<int64_t>(N), out_index, out_neigh);
    pvector<uint8_t> matrix;
    const auto encoding = argc > base + 2 * E
        ? popt_reref::Encoding::SingleEpoch : popt_reref::Encoding::Full;
    makeOffsetMatrix(g, matrix, numVtxPerLine, numEpochs, true, encoding);
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(matrix.data()),
              static_cast<std::streamsize>(matrix.size()));
    return 0;
}
'''


@pytest.fixture(scope="module")
def popt_dump_binary(tmp_path_factory) -> Path:
    """Compile the popt-dump harness once per test module."""

    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ not available on PATH")

    workdir = tmp_path_factory.mktemp("popt_dump_build")
    source = workdir / "popt_dump.cc"
    binary = workdir / "popt_dump"
    source.write_text(POPT_DUMP_CC)

    cmd = [
        compiler,
        "-std=c++17",
        "-O2",
        "-fopenmp",
        "-DNO_M5OPS",
        f"-I{PROJECT_ROOT / 'bench/include/external/gapbs'}",
        f"-I{PROJECT_ROOT / 'bench/include'}",
        f"-I{PROJECT_ROOT / 'bench/include/graphbrew'}",
        str(source),
        "-o",
        str(binary),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr[-4000:]
    return binary


def _run_popt_dump(
    binary: Path,
    tmp_path: Path,
    *,
    n: int,
    vtx_per_line: int,
    epochs: int,
    edges: Sequence[tuple[int, int]],
    single_epoch: bool = False,
) -> bytes:
    out = tmp_path / "matrix.bin"
    cmd = [str(binary), str(out), str(vtx_per_line), str(epochs), str(n), str(len(edges))]
    for s, d in edges:
        cmd.extend([str(s), str(d)])
    if single_epoch:
        cmd.append("single_epoch")
    result = subprocess.run(
        cmd, capture_output=True, text=True, timeout=60,
        env={**os.environ, "OMP_NUM_THREADS": "1"})
    assert result.returncode == 0, (
        f"popt_dump exited rc={result.returncode}\nstderr:\n{result.stderr[-400:]}"
    )
    assert out.exists(), f"matrix file not produced: {out}"
    return out.read_bytes()


def _adj_from_edges(n: int, edges: Sequence[tuple[int, int]]) -> list[list[int]]:
    adj: list[list[int]] = [[] for _ in range(n)]
    for s, d in edges:
        adj[s].append(d)
    for v in range(n):
        adj[v].sort()
    return adj


# ---------------------------------------------------------------------------
# Parity tests across small synthetic graphs.
# ---------------------------------------------------------------------------


GRAPH_CASES = [
    pytest.param(
        4,
        1,
        [(0, 1), (1, 2), (2, 3), (3, 0), (0, 2)],
        id="square_with_diag_vpl1",
    ),
    pytest.param(
        8,
        2,
        [(0, 1), (0, 3), (1, 2), (1, 4), (2, 5), (3, 4), (4, 5), (5, 6), (6, 7), (7, 0)],
        id="ring_with_chords_vpl2",
    ),
    pytest.param(
        16,
        4,
        [
            (0, 4), (1, 5), (2, 6), (3, 7),
            (4, 8), (5, 9), (6, 10), (7, 11),
            (8, 12), (9, 13), (10, 14), (11, 15),
            (0, 15), (15, 0), (3, 12), (12, 3),
        ],
        id="bipartite_chain_vpl4",
    ),
]


def single_epoch_reference(n, edges, vertices_per_line):
    """Derive bytes from sorted use sets, independently of the C++ passes."""
    lines = (n + vertices_per_line - 1) // vertices_per_line
    epoch_size = (n + 255) // 256
    sub_size = (epoch_size + 63) // 64
    uses = [dict() for _ in range(lines)]
    for source, destination in edges:
        epoch, position = divmod(destination, epoch_size)
        line = source // vertices_per_line
        uses[line][epoch] = max(uses[line].get(epoch, 0), position)
    result = bytearray(lines * 256)
    for line, positions in enumerate(uses):
        for epoch in range(256):
            flag = 0x40 if epoch + 1 in positions else 0
            if epoch in positions:
                payload = positions[epoch] // sub_size
            else:
                later = [e - epoch for e in positions if e > epoch]
                payload = 0x80 | min(min(later, default=63), 63)
            result[epoch * lines + line] = flag | payload
    return bytes(result)


@pytest.mark.parametrize("n,vtx_per_line", [(1025, 4), (40961, 256)])
def test_single_epoch_matrix_matches_independent_reference(
        popt_dump_binary, tmp_path, n, vtx_per_line):
    rng = random.Random(420)
    edges = [(rng.randrange(n), rng.randrange(n)) for _ in range(1500)]
    edges.extend([(0, 0), (0, n - 1), (n - 1, n - 1)])
    actual = _run_popt_dump(
        popt_dump_binary, tmp_path, n=n, vtx_per_line=vtx_per_line,
        epochs=256, edges=edges, single_epoch=True)
    assert actual == single_epoch_reference(n, edges, vtx_per_line)


def test_single_epoch_reference_hand_computed_entries():
    matrix = single_epoch_reference(
        65536, [(0, 20), (0, 276), (16, 511), (32, 65535)], 16)
    lines = 4096
    assert matrix[0] == 0x45
    assert matrix[lines] == 0x05
    assert matrix[1] == 0xc1
    assert matrix[lines + 1] == 0x3f
    assert matrix[2] == 0xbf
    assert matrix[255 * lines + 2] == 0x3f
    assert matrix[3] == 0xbf


def test_single_epoch_cpp_regressions(tmp_path):
    compiler = shutil.which("g++")
    if compiler is None:
        pytest.skip("g++ not available on PATH")
    binary = tmp_path / "popt_single_epoch"
    result = subprocess.run([
        compiler, "-std=c++17", "-O2", "-fopenmp",
        f"-I{PROJECT_ROOT / 'bench/include'}",
        str(PROJECT_ROOT / "bench/src_sim/test_popt_single_epoch.cc"),
        "-o", str(binary),
    ], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr[-4000:]
    result = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=30,
        env={**os.environ, "OMP_NUM_THREADS": "1"})
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("header", [
    "bench/include/popt_rereference.h",
    "bench/include/graphbrew/partition/cagra/popt.h",
])
def test_popt_headers_are_sim_build_dependencies(header):
    result = subprocess.run(
        ["make", "-qp", "bench/bin_sim/pr"], cwd=PROJECT_ROOT,
        capture_output=True, text=True, timeout=30)
    assert result.returncode in (0, 1), result.stderr
    rule = next(line for line in result.stdout.splitlines()
                if line.startswith("bench/bin_sim/pr:"))
    assert header in rule.split(), f"{header} must invalidate the PR binary"


@pytest.mark.parametrize("n,vtx_per_line,edges", GRAPH_CASES)
def test_popt_matrix_matches_reference(popt_dump_binary, tmp_path, n, vtx_per_line, edges):
    """C++ ``makeOffsetMatrix`` output must be bit-exact equal to the Python
    reference encoding of the same input graph."""

    cpp_matrix = _run_popt_dump(
        popt_dump_binary,
        tmp_path,
        n=n,
        vtx_per_line=vtx_per_line,
        epochs=256,
        edges=edges,
    )
    adj = _adj_from_edges(n, edges)
    py_matrix = make_offset_matrix_reference(n, adj, vtx_per_line, num_epochs=256)

    num_cache_lines = (n + vtx_per_line - 1) // vtx_per_line
    expected_bytes = num_cache_lines * 256
    assert len(cpp_matrix) == expected_bytes, (
        f"matrix size mismatch: cpp={len(cpp_matrix)} expected={expected_bytes}"
    )
    assert len(py_matrix) == expected_bytes, (
        f"python ref size mismatch: py={len(py_matrix)} expected={expected_bytes}"
    )
    assert py_matrix == cpp_matrix, (
        "Tier B: makeOffsetMatrix output diverged from the upstream P-OPT "
        f"encoding (n={n}, vpl={vtx_per_line}). First differing byte at index "
        f"{next((i for i,(a,b) in enumerate(zip(py_matrix, cpp_matrix)) if a!=b), -1)}"
    )


def test_popt_matrix_isolated_vertices_use_max_distance_sentinel(popt_dump_binary, tmp_path):
    """Cache lines that never reach a referenced epoch encode the capped
    sentinel ``MAX_REREF | OR_MASK = 0xFF`` for every epoch.  Locks the
    MSB-polarity convention against accidental sign flips."""

    # 4 isolated nodes — no edges at all.
    matrix = _run_popt_dump(
        popt_dump_binary, tmp_path, n=4, vtx_per_line=1, epochs=256, edges=[(0, 0)]
    )
    # Cache line 0 references itself once in epoch 0; cache lines 1/2/3 never
    # see any reference and must therefore be entirely 0xFF.
    assert len(matrix) == 4 * 256
    for c in (1, 2, 3):
        column = bytes(matrix[e * 4 + c] for e in range(256))
        assert set(column) == {0xFF}, (
            f"isolated cache line {c} must encode 0xFF everywhere, got "
            f"{sorted(set(column))}"
        )


def test_popt_matrix_referenced_epochs_clear_msb(popt_dump_binary, tmp_path):
    """Any cache line that owns the destination of any edge has at least one
    MSB=0 (referenced) entry in its column."""

    n = 4
    vpl = 1
    edges = [(0, 1), (1, 2), (2, 3), (3, 0)]
    matrix = _run_popt_dump(
        popt_dump_binary, tmp_path, n=n, vtx_per_line=vpl, epochs=256, edges=edges
    )
    num_cache_lines = (n + vpl - 1) // vpl
    for c in range(num_cache_lines):
        column = [matrix[e * num_cache_lines + c] for e in range(256)]
        msb_clear_count = sum(1 for b in column if (b & 0x80) == 0)
        assert msb_clear_count >= 1, (
            f"cache line {c} has no referenced epoch (no MSB=0 byte) — every "
            "byte indicates 'no future reference', but the edge list pulls "
            f"this line as a destination."
        )


def test_python_reference_matches_handwritten_invariants():
    """Sanity-check the Python reference against analytically-derived values
    for a 4-node graph with one edge.  Keeps the reference honest in case it
    drifts.  N=4, vpl=1, single edge 0→3 means:

    * Cache line 0 references vertex 3 in epoch 3.  Encoding: sub_pos=0,
      MSB=0 → ``compressed[0][3] = 0x00``; forward-distance scan rewrites
      ``[0][0..2]`` to (3, 2, 1) | 0x80.  ``[0][4..255]`` stay 0xFF.
    * Cache lines 1, 2, 3 never reference anything → all 0xFF.
    """

    adj = [[3], [], [], []]
    m = make_offset_matrix_reference(4, adj, 1, 256)
    # Transposed layout: byte at epoch e, cache line c is m[e*4 + c].
    assert m[0 * 4 + 0] == (3 | 0x80), hex(m[0 * 4 + 0])
    assert m[1 * 4 + 0] == (2 | 0x80), hex(m[1 * 4 + 0])
    assert m[2 * 4 + 0] == (1 | 0x80), hex(m[2 * 4 + 0])
    assert m[3 * 4 + 0] == 0x00, hex(m[3 * 4 + 0])
    # Epochs 4..255 of cache line 0 are unreachable → 0xFF.
    for e in range(4, 256):
        assert m[e * 4 + 0] == 0xFF, (e, hex(m[e * 4 + 0]))
    # Cache lines 1, 2, 3 carry no references at all.
    for c in (1, 2, 3):
        for e in range(256):
            assert m[e * 4 + c] == 0xFF, (c, e, hex(m[e * 4 + c]))
