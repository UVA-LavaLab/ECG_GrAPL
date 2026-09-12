"""Validation and aggregate rows for the existing runner's independent-query path."""

from __future__ import annotations

if __package__:
    from . import algorithm_matrix as algorithms
else:
    import algorithm_matrix as algorithms

FIELDS = ("total_accesses", "memory_accesses", "prefetch_fills", "llc_writebacks",
          "llc_hits", "llc_misses", "llc_property_hits", "llc_property_misses", "total_offchip_traffic")
ADDITIVE_WORK = ("passes", "structural_positions", "actual_records", "skipped_positions",
                 "csr_index_reads", "edge_reads", "weight_reads", "ordinary_property_reads",
                 "property_writes", "auxiliary_accesses", "construction_read_bytes",
                 "construction_write_bytes", "bindings")


def counters(value):
    algorithms.require(isinstance(value, dict), "missing batch traffic counters")
    result = {field: algorithms._integer(value, field) for field in FIELDS}
    algorithms.require(
        result["total_offchip_traffic"] == result["memory_accesses"] + result["llc_writebacks"] and
        result["prefetch_fills"] == 0 and result["llc_misses"] == result["memory_accesses"] and
        result["llc_property_hits"] <= result["llc_hits"] and
        result["llc_property_misses"] <= result["llc_misses"],
        "invalid independent-query traffic accounting")
    return result


def add(*parts):
    return {field: sum(part[field] for part in parts) for field in FIELDS}


def validate_batch(payload, log, *, graph, options, policy, evidence, llc_bytes, llc_ways):
    require = algorithms.require
    integer = algorithms._integer
    require(payload.get("schema") == "ecg.spmv-queries.v1" and payload.get("backend") == "cache_sim" and
            payload.get("measurement_scope") == "shared-graph-batch-data-traffic" and
            payload.get("timing_valid_for_speedup") is False and
            payload.get("graph_storage_reused") is True and payload.get("cache_state_preserved") is True and
            payload.get("properties") == "fresh-query-local-arrays" and
            payload.get("serialized_loading") == "excluded-as-in-single-query-controls" and
            payload.get("policy") == policy and integer(payload, "query_count") == options.queries,
            "independent-query batch identity or ownership receipt mismatch")
    require(1 < options.queries <= 64 and not graph.weighted, "unsupported independent-query workload")
    queries = payload.get("queries")
    require(isinstance(queries, list) and len(queries) == options.queries, "incomplete query batch")
    zero = {field: 0 for field in FIELDS}
    cumulative, preparation, setups, kernels = zero.copy(), zero.copy(), zero.copy(), zero.copy()
    works = []
    matrix = policy == "POPT_UNCHARGED"
    stable_matrix = ("matrix_digest", "matrix_bytes", "matrix_lines", "epochs", "banks", "covered_regions",
                     "encoding", "scope", "full_data_capacity", "runtime_matrix_traffic_charged",
                     "consumer", "role", "rank_mode")
    for index, query in enumerate(queries):
        require(isinstance(query, dict) and integer(query, "query_index") == index + 1 and
                query.get("metrics_scope") == "query-delta-LLC-and-offchip",
                "query order or metric scope mismatch")
        work = algorithms.validate_payload(query, log, algorithm="spmv", mode="csr", policy=policy,
            graph=graph, graph_path=options.graph, options=options, requested_bytes=0,
            minimum_mantissa_bits=0, evidence=evidence, llc_sets=llc_bytes // (64 * llc_ways),
            allow_reused_popt=matrix and index > 0)
        algorithms.validate_traffic_phases(query)
        algorithms.validate_bfs_phases(query, options)
        require(not works or work == works[0], "independent queries changed fixed-input SpMV work or results")
        works.append(work)
        llc = query["metrics"]["L3"]
        require(integer(llc, "size_bytes") == llc_bytes and integer(llc, "ways") == llc_ways and
                integer(query["metrics"], "popt_matrix_stream_lines_simulated") == 0,
                "query lost nominal capacity or enabled matrix-stream charges")
        p, s, k = (counters(query["graph_preparation"]), counters(query["query_setup"]),
                   counters(query["traffic_phases"]["kernel"]))
        require(add(p, s) == counters(query["traffic_phases"]["setup"]),
                "one-time preparation and query setup do not close")
        require((matrix and index == 0) or p == zero, "graph preparation was repeated or unexpected")
        cumulative = add(cumulative, p, s, k)
        require(cumulative == counters(query["cumulative_traffic"]), "query prefix counters do not close")
        preparation, setups, kernels = add(preparation, p), add(setups, s), add(kernels, k)
        if matrix:
            popt = query["popt"]
            require(popt.get("reused") is (index > 0) and integer(popt, "construction_count") == 1 and
                    integer(popt, "owner_reservation_bytes") == 512 and
                    all(popt.get(key) == queries[0]["popt"].get(key) for key in stable_matrix),
                    "P-OPT matrix was rebuilt or changed across queries")
            if index == 0:
                byte_work = integer(popt, "construction_read_bytes") + integer(popt, "construction_write_bytes")
                require(0 < p["total_accesses"] <= byte_work <= 64 * p["total_accesses"],
                        "one-time matrix work is missing from preparation traffic")
    require(integer(payload, "matrix_constructions") == int(matrix) and
            integer(payload, "shared_owner_reservation_bytes") == (512 if matrix else 0) and
            integer(payload, "shared_matrix_bytes") == (queries[0]["popt"]["matrix_bytes"] if matrix else 0),
            "shared matrix ownership receipt mismatch")
    for key, expected in (("graph_preparation", preparation), ("query_setup", setups),
                          ("query_kernel", kernels), ("total_traffic", cumulative)):
        require(counters(payload[key]) == expected, f"batch total does not close: {key}")
    phases = {"boundary": "first-binding-complete", "cache_state_preserved": True,
              "setup": add(preparation, setups), "kernel": kernels}
    result = algorithms.validate_traffic_phases({"metrics": payload["metrics"], "traffic_phases": phases})
    require(integer(payload["metrics"]["L3"], "size_bytes") == llc_bytes and
            integer(payload["metrics"]["L3"], "ways") == llc_ways and
            integer(payload["metrics"], "popt_matrix_stream_lines_simulated") == 0,
            "batch geometry or favorable P-OPT scope mismatch")
    work = dict(works[0])
    for field in ADDITIVE_WORK:
        work[field] = sum(integer(value, field) for value in works)
    result.update({"algorithm_" + key: value for key, value in work.items()
                   if key != "algorithm" and not key.startswith("values_")})
    result.update(measurement_scope=payload["measurement_scope"], query_count=options.queries,
        algorithm_result_digest_scope="each-identical-query", algorithm_work_digest_scope="each-identical-query",
        setup_cache_policy=queries[0]["setup_cache_policy"],
        record_base_policy="LRU", host_seconds=payload["host_seconds"],
        total_accesses=cumulative["total_accesses"], memory_accesses=cumulative["memory_accesses"],
        total_offchip_traffic=cumulative["total_offchip_traffic"], llc_writebacks=cumulative["llc_writebacks"],
        prefetch_fills=0, l3_misses=cumulative["llc_misses"], l3_hits=cumulative["llc_hits"],
        l3_accesses=cumulative["llc_hits"] + cumulative["llc_misses"],
        matrix_constructions=payload["matrix_constructions"], shared_matrix_bytes=payload["shared_matrix_bytes"],
        shared_owner_reservation_bytes=payload["shared_owner_reservation_bytes"])
    result["l3_miss_rate"] = result["l3_misses"] / result["l3_accesses"] if result["l3_accesses"] else 0
    for prefix, traffic in (("graph_preparation_", preparation), ("query_setup_", setups)):
        result.update({prefix + key: value for key, value in traffic.items()})
    return result
