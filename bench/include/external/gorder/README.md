# gorder/ — GOrder Reference Implementation

Original GoGraph-based GOrder from [Hao Wei et al.](https://github.com/datourat/Gorder)
Uses a custom adjacency format and `UnitHeap` priority queue.

Activated via `-o 9` (default variant).

CSR-native reimplementations (`-o 9:csr`, `-o 9:fast`) live in
`bench/include/graphbrew/reorder/reorder_gorder.h`.
