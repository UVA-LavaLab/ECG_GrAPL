import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.experiments.ecg.analysis.k2_area import K2AreaConfig, model  # noqa: E402


def test_minimum_and_contextual_k2_area():
    result = model(K2AreaConfig())

    assert result["lines"] == 131_072
    assert result["minimum_line_bits"] == 33
    assert result["configured_line_bits"] == 49
    assert result["minimum_bit_packed_payload_bytes"] == 540_672
    assert result["configured_bit_packed_payload_bytes"] == 802_816
    assert result["minimum_data_bit_ratio"] == 0.064453125
    assert result["configured_data_bit_ratio"] == 0.095703125
    assert result["minimum_baseline_way_equivalent"] == 1.03125
    assert result["configured_baseline_way_equivalent"] == 1.53125
    assert result["minimum_equal_area_fractional_ways"] == 8192 / 545
    assert result["configured_equal_area_fractional_ways"] == 8192 / 561
    assert result["first_sensitivity_ways"] == 15
    assert result["first_sensitivity_area_ratio"] == 8415 / 8192
    assert result["max_integral_equal_area_ways"] == 14
    assert result["max_integral_area_ratio"] == 7854 / 8192


def test_minimum_state_allows_fifteen_equal_area_ways():
    result = model(K2AreaConfig(context_bits=0))

    assert result["configured_line_bits"] == 33
    assert result["configured_equal_area_fractional_ways"] > 15
    assert result["max_integral_equal_area_ways"] == 15


def test_request_and_csr_state_are_explicit():
    result = model(K2AreaConfig())

    assert result["logical_request_payload_bits"] == 95
    assert result["mshr_conflict_bits"] == 1
    assert result["per_hart_epoch_context_csr_bits"] == 31
    assert result["per_hart_sequence_counter_bits"] == 32
    assert result["streamshield_request_bits"] == 1


def test_invalid_cache_geometry_fails():
    try:
        model(K2AreaConfig(cache_bytes=1000, line_bytes=64))
    except ValueError as error:
        assert "divisible" in str(error)
    else:
        raise AssertionError("invalid cache geometry must fail")


def test_negative_bit_width_fails():
    try:
        model(K2AreaConfig(context_bits=-1))
    except ValueError as error:
        assert "non-negative" in str(error)
    else:
        raise AssertionError("negative bit widths must fail")


def test_no_integral_equal_area_cache_is_reported_as_zero_ways():
    result = model(K2AreaConfig(ways=1))

    assert result["first_sensitivity_ways"] == 0
    assert result["max_integral_equal_area_ways"] == 0
    assert result["max_integral_area_ratio"] == 0
