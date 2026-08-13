"""Static-invariant test for GRASP property-array registration.

This test enforces the rule that surfaced from the BC multi-property GRASP
registration bug:

  In any cache_sim benchmark that registers more than one
  vertex-indexed property array via ``registerPropertyArray()``,
  *all* such arrays must be marked ``grasp_region=true``.

The motivation:

* ``GraphCacheContext::classifyGRASP()`` (graph_cache_context.h) walks
  every region with ``grasp_region == true`` and applies the hot/moderate
  boundary inside it.
* If a benchmark has, say, four property arrays but only one is marked as
  a GRASP region, the other three thrash under SRRIP while the one
  protected array hogs the LLC.  On web-Google/BC this cost ≈20 pp of L3
  hit-rate vs LRU and made GRASP look catastrophically worse than
  literature reports.
* The opposite case (all-false) is intentionally allowed because it
  configures a baseline that disables GRASP region handling for the
  benchmark; it's the *mixed* configuration that is almost certainly a
  bug.

We parse the source with regex (good enough for these small files; the
calls are always single-line).  Test fails fast with the offending file
and the boolean breakdown so the fix is obvious.
"""
from __future__ import annotations

import re
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SRC_DIRS = [
    REPO_ROOT / "bench" / "src_sim",
    REPO_ROOT / "bench" / "src_gem5",
    REPO_ROOT / "bench" / "src_sniper",
]

CALL_RE = re.compile(
    r"registerPropertyArray\s*\(([^;]*?)\)\s*;",
    re.DOTALL,
)

# For gem5/Sniper, property regions are aggregated as struct-initializer
# arrays — capture each {"name", ...} initializer.  The last field of the
# struct (after the closing brace's trailing comma) is `grasp_region`.
STRUCT_REGION_RE = re.compile(
    r"\{\s*\"[^\"]+\"\s*,[^{}]*?\}",
    re.DOTALL,
)


def _parse_grasp_region(call_args: str) -> bool | None:
    """Return the value of the trailing ``grasp_region`` argument.

    The signature is::

        void registerPropertyArray(const void* data_ptr, uint32_t num_elements,
                                   uint32_t elem_size, size_t llc_size,
                                   double manual_hot_fraction = -1.0,
                                   bool grasp_region = true);

    If fewer than 6 positional arguments are supplied the default
    (``true``) applies.
    """
    args: list[str] = []
    depth = 0
    current: list[str] = []
    for ch in call_args:
        if ch == "(":
            depth += 1
            current.append(ch)
        elif ch == ")":
            depth -= 1
            current.append(ch)
        elif ch == "," and depth == 0:
            args.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    if current:
        args.append("".join(current).strip())
    if len(args) < 6:
        return True
    val = args[5].strip()
    if val.lower() == "true":
        return True
    if val.lower() == "false":
        return False
    return None


def _scan(path: Path) -> list[bool | None]:
    text = path.read_text()
    # Strip line comments to avoid matching examples in comments.
    text = re.sub(r"//[^\n]*", "", text)
    out: list[bool | None] = []
    for match in CALL_RE.finditer(text):
        out.append(_parse_grasp_region(match.group(1)))
    # For gem5/Sniper sources the property regions are represented as
    # struct initializers (Gem5PropertyRegion / SniperPropertyRegion).
    # Extract the trailing boolean field of every such initializer that
    # looks like a property region declaration.
    if "Gem5PropertyRegion" in text or "SniperPropertyRegion" in text:
        # Both struct types have 6 fields with grasp_region defaulting to
        # true.  When the brace-init supplies only 5 fields the default
        # applies — treat that as True rather than "unparseable".
        for region in STRUCT_REGION_RE.finditer(text):
            body = region.group(0)
            inner = body.strip().strip("{}").strip()
            # Top-level split (ignore commas inside nested parens like
            # reinterpret_cast<uint64_t>(...) and casts).
            fields: list[str] = []
            depth = 0
            buf: list[str] = []
            for ch in inner:
                if ch in "(<":
                    depth += 1
                    buf.append(ch)
                elif ch in ")>":
                    depth -= 1
                    buf.append(ch)
                elif ch == "," and depth == 0:
                    fields.append("".join(buf).strip())
                    buf = []
                else:
                    buf.append(ch)
            if buf:
                fields.append("".join(buf).strip())
            if len(fields) < 5:
                continue  # not a property-region initializer
            if len(fields) == 5:
                out.append(True)  # default applies
                continue
            tail = fields[-1].strip().lower()
            if tail == "true":
                out.append(True)
            elif tail == "false":
                out.append(False)
            else:
                out.append(None)
    return out


_SRC_FILES: list[Path] = []
for d in SRC_DIRS:
    if d.exists():
        _SRC_FILES.extend(sorted(d.glob("*.cc")))


@pytest.mark.parametrize(
    "src_file", _SRC_FILES,
    ids=[f"{p.parent.name}/{p.name}" for p in _SRC_FILES],
)
def test_multi_property_grasp_region_consistent(src_file: Path) -> None:
    flags = _scan(src_file)
    if len(flags) <= 1:
        pytest.skip(f"{src_file.name} registers ≤1 property array")
    # Every entry must have parsed cleanly so the test catches malformed
    # registrations as well.
    assert all(f is not None for f in flags), (
        f"{src_file.name}: could not parse grasp_region flag in one or "
        f"more registerPropertyArray() calls: {flags}"
    )
    distinct = set(flags)
    assert len(distinct) == 1, (
        f"{src_file.name} registers {len(flags)} property arrays with "
        f"mixed grasp_region flags {flags}. All must be true (preferred) "
        f"or all false. Mixed configuration caused the BC multi-property "
        f"GRASP registration bug."
    )


def test_popt_uses_all_registered_property_regions() -> None:
    paths = [
        REPO_ROOT / "bench/include/cache_sim/cache_sim.h",
        REPO_ROOT / (
            "bench/include/gem5_sim/overlays/mem/cache/"
            "replacement_policies/popt_rp.cc"),
        REPO_ROOT / (
            "bench/include/sniper_sim/overlays/common/core/"
            "memory_subsystem/cache/cache_set_popt.cc"),
    ]
    for path in paths:
        text = path.read_text()
        if path.name == "cache_sim.h":
            text = text.split("size_t findVictimPOPT", 1)[1].split(
                "size_t findVictimECG", 1)[0]
        assert "isPropertyData" in text
        assert "isEcgEpochData" not in text


def test_bc_registers_the_same_four_property_arrays_in_all_backends() -> None:
    expected = {"scores", "depth", "path_counts", "deltas"}
    for path in [
        REPO_ROOT / "bench/src_gem5/bc.cc",
        REPO_ROOT / "bench/src_sniper/sg_kernel.cc",
    ]:
        text = path.read_text()
        assert all(f'"{name}"' in text for name in expected)

    cache_sim = (REPO_ROOT / "bench/src_sim/bc.cc").read_text()
    assert all(f"registerPropertyArray({name}" in cache_sim for name in [
        "scores.data()", "depths.data()", "path_counts.data()", "deltas.data()"])
