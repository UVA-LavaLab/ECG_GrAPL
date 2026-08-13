# rabbit/ — RabbitOrder Reference Implementation

Community-based graph ordering via hierarchical clustering from
[araij/rabbit_order](https://github.com/araij/rabbit_order).

| Header | Purpose |
|--------|---------|
| `rabbit_order.hpp` | Core RabbitOrder algorithm (Boost-based) |
| `edge_list.hpp` | Edge list utilities |

Activated via `-o 8:boost`. Kept as a reference baseline for validation.
The CSR-native reimplementation (`-o 8` / `-o 8:csr`) lives in
`bench/include/graphbrew/reorder/reorder_rabbit.h` and is recommended for
production use (4–40% better cache locality, auto-adaptive resolution, no
external dependencies).
