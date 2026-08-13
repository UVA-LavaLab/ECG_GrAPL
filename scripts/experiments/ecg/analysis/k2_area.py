#!/usr/bin/env python3
"""Analytical K2 metadata and equal-area accounting."""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import asdict, dataclass


@dataclass(frozen=True)
class K2AreaConfig:
    cache_bytes: int = 8 * 1024 * 1024
    line_bytes: int = 64
    ways: int = 16
    epoch_bits: int = 15
    tier_bits: int = 2
    valid_bits: int = 1
    context_bits: int = 16
    metadata_ecc_bits: int = 0
    sequence_bits: int = 32


def model(config: K2AreaConfig) -> dict[str, int | float]:
    if config.cache_bytes <= 0:
        raise ValueError("cache_bytes must be positive")
    if config.line_bytes <= 0 or config.ways <= 0:
        raise ValueError("line_bytes and ways must be positive")
    if config.cache_bytes % config.line_bytes:
        raise ValueError("cache_bytes must be divisible by line_bytes")
    bit_fields = (
        config.epoch_bits,
        config.tier_bits,
        config.valid_bits,
        config.context_bits,
        config.metadata_ecc_bits,
        config.sequence_bits,
    )
    if any(bits < 0 for bits in bit_fields):
        raise ValueError("bit widths must be non-negative")

    lines = config.cache_bytes // config.line_bytes
    minimum_line_bits = (
        2 * config.epoch_bits + config.tier_bits + config.valid_bits)
    implemented_line_bits = (
        minimum_line_bits + config.context_bits + config.metadata_ecc_bits)
    data_line_bits = config.line_bytes * 8
    minimum_ratio = minimum_line_bits / data_line_bits
    implemented_ratio = implemented_line_bits / data_line_bits
    minimum_way_equivalent = minimum_ratio * config.ways
    implemented_way_equivalent = implemented_ratio * config.ways
    minimum_equal_area_fractional_ways = (
        config.ways * data_line_bits /
        (data_line_bits + minimum_line_bits))
    configured_equal_area_fractional_ways = (
        config.ways * data_line_bits /
        (data_line_bits + implemented_line_bits))
    first_sensitivity_ways = max(config.ways - 1, 0)
    first_sensitivity_area_ratio = (
        first_sensitivity_ways * (data_line_bits + implemented_line_bits) /
        (config.ways * data_line_bits))
    max_integral_equal_area_ways = math.floor(
        configured_equal_area_fractional_ways)
    max_integral_area_ratio = (
        max_integral_equal_area_ways *
        (data_line_bits + implemented_line_bits) /
        (config.ways * data_line_bits))

    request_bits = (
        2 * config.epoch_bits
        + config.tier_bits
        + config.epoch_bits
        + config.context_bits
        + config.sequence_bits
    )

    return {
        **asdict(config),
        "lines": lines,
        "minimum_line_bits": minimum_line_bits,
        "configured_line_bits": implemented_line_bits,
        "minimum_bit_packed_payload_bytes": math.ceil(
            lines * minimum_line_bits / 8),
        "configured_bit_packed_payload_bytes": math.ceil(
            lines * implemented_line_bits / 8),
        "minimum_data_bit_ratio": minimum_ratio,
        "configured_data_bit_ratio": implemented_ratio,
        "minimum_baseline_way_equivalent": minimum_way_equivalent,
        "configured_baseline_way_equivalent": implemented_way_equivalent,
        "minimum_equal_area_fractional_ways":
            minimum_equal_area_fractional_ways,
        "configured_equal_area_fractional_ways":
            configured_equal_area_fractional_ways,
        "first_sensitivity_ways": first_sensitivity_ways,
        "first_sensitivity_area_ratio": first_sensitivity_area_ratio,
        "max_integral_equal_area_ways": max_integral_equal_area_ways,
        "max_integral_area_ratio": max_integral_area_ratio,
        "logical_request_payload_bits": request_bits,
        "mshr_conflict_bits": 1,
        "per_hart_epoch_context_csr_bits":
            config.epoch_bits + config.context_bits,
        "per_hart_sequence_counter_bits": config.sequence_bits,
        "streamshield_request_bits": 1,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compute K2 metadata and equal-area sensitivity.")
    parser.add_argument("--cache-bytes", type=int, default=8 * 1024 * 1024)
    parser.add_argument("--line-bytes", type=int, default=64)
    parser.add_argument("--ways", type=int, default=16)
    parser.add_argument("--epoch-bits", type=int, default=15)
    parser.add_argument("--context-bits", type=int, default=16)
    parser.add_argument("--metadata-ecc-bits", type=int, default=0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result = model(K2AreaConfig(
        cache_bytes=args.cache_bytes,
        line_bytes=args.line_bytes,
        ways=args.ways,
        epoch_bits=args.epoch_bits,
        context_bits=args.context_bits,
        metadata_ecc_bits=args.metadata_ecc_bits,
    ))
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
