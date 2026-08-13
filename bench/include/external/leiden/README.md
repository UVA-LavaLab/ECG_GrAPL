# leiden/ — GVE-Leiden Community Detection

OpenMP-parallel Leiden community detection from
[puzzlef/leiden-communities-openmp](https://github.com/puzzlef/leiden-communities-openmp).

Used by:
- **LeidenOrder** (`-o 15`) — standalone Leiden baseline
- **GraphBrewOrder** (`-o 12`) — Leiden as the community detection layer

Header-only library; main entry point is `leiden.hxx`.
