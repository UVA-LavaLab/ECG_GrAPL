"""Canonical ECG experiment policy parsing and output labels."""

from __future__ import annotations

import re
from dataclasses import dataclass


ONLINE_DUELING_WINDOW_MISSES = 1024
ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS = (
    "gem5_k2_dueling_request_bound_victims",
    "gem5_k2_dueling_leader_samples",
    "gem5_k2_dueling_follower_selections",
    "gem5_k2_dueling_completed_windows",
)
ONLINE_DUELING_REPORTED_FIELDS = (
    *ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS,
    "gem5_k2_dueling_winner_changes",
    "gem5_k2_dueling_follower_variant_overrides",
)

# Sniper analog of the gem5 online-dueling evidence above. Field names use a
# "sniper_" prefix and "governed_victims" rather than gem5's frozen
# "request_bound_victims": Sniper has no O3 Request/MSHR-attested victim to
# bind to, so its population is the closest Sniper-equivalent (a
# marker/sideband-governed miss population; see cache_set_ecg.cc's
# OnlineDuelingEvidence comment and sniper_k2_dueling_binding_model). The
# frozen gem5_* fields above are never renamed or repurposed for Sniper.
SNIPER_ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS = (
    "sniper_k2_dueling_governed_victims",
    "sniper_k2_dueling_leader_samples",
    "sniper_k2_dueling_follower_selections",
    "sniper_k2_dueling_completed_windows",
)
SNIPER_ONLINE_DUELING_REPORTED_FIELDS = (
    *SNIPER_ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS,
    "sniper_k2_dueling_winner_changes",
    "sniper_k2_dueling_follower_variant_overrides",
)


@dataclass(frozen=True)
class PolicySpec:
    label: str
    policy: str
    ecg_mode: str | None = None
    charge_popt_overhead: bool = False
    ecg_schedule_k: int = 0
    ecg_stream_bypass: bool = False
    ecg_stream_adaptive: bool = False
    ecg_variant: str | None = None
    ecg_transport_pinned: bool = False
    ecg_set_dueling: bool = False

    @property
    def safe_label(self) -> str:
        return re.sub(r"[^A-Za-z0-9_.-]+", "_", self.label)


def parse_policy_spec(text: str) -> PolicySpec:
    upper = text.strip().upper().replace("-", "_")
    charge_popt = False
    explicit_charge = False
    if upper.endswith("_CHARGED"):
        upper = upper[: -len("_CHARGED")]
        charge_popt = True
        explicit_charge = True
    elif upper.endswith(":CHARGED"):
        upper = upper[: -len(":CHARGED")]
        charge_popt = True
        explicit_charge = True
    elif upper.endswith("_UNCHARGED"):
        upper = upper[: -len("_UNCHARGED")]
        explicit_charge = True
    elif upper.endswith(":UNCHARGED"):
        upper = upper[: -len(":UNCHARGED")]
        explicit_charge = True

    if upper in ("ECG:K2", "ECG_K2"):
        return PolicySpec(
            label="ECG_K2",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_schedule_k=2,
            ecg_variant="adaptive",
            ecg_transport_pinned=True,
        )
    k2_variants = {
        "ECG:K2_GRASP": ("ECG_K2_GRASP", "grasp_only"),
        "ECG_K2_GRASP": ("ECG_K2_GRASP", "grasp_only"),
        "ECG:K2_EPOCH": ("ECG_K2_EPOCH", "epoch_first"),
        "ECG_K2_EPOCH": ("ECG_K2_EPOCH", "epoch_first"),
        "ECG:K2_RRIP": ("ECG_K2_RRIP", "rrip_first"),
        "ECG_K2_RRIP": ("ECG_K2_RRIP", "rrip_first"),
        "ECG:K2_DEGREE": ("ECG_K2_DEGREE", "degree_first"),
        "ECG_K2_DEGREE": ("ECG_K2_DEGREE", "degree_first"),
        "ECG:K2_LRU": ("ECG_K2_LRU", "lru_only"),
        "ECG_K2_LRU": ("ECG_K2_LRU", "lru_only"),
    }
    if upper in k2_variants:
        label, variant = k2_variants[upper]
        return PolicySpec(
            label=label,
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_schedule_k=2,
            ecg_variant=variant,
            ecg_transport_pinned=True,
        )
    if upper in (
        "ECG:K2_RRIP_STREAMSHIELD",
        "ECG_K2_RRIP_STREAMSHIELD",
        "ECG:K2_RRIP_SS",
        "ECG_K2_RRIP_SS",
    ):
        return PolicySpec(
            label="ECG_K2_RRIP_STREAMSHIELD",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_schedule_k=2,
            ecg_stream_bypass=True,
            ecg_variant="rrip_first",
            ecg_transport_pinned=True,
        )
    if upper in (
        "ECG:K2_LRU_STREAMSHIELD",
        "ECG_K2_LRU_STREAMSHIELD",
        "ECG:K2_LRU_SS",
        "ECG_K2_LRU_SS",
    ):
        return PolicySpec(
            label="ECG_K2_LRU_STREAMSHIELD",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_schedule_k=2,
            ecg_stream_bypass=True,
            ecg_variant="lru_only",
            ecg_transport_pinned=True,
        )
    if upper in (
        "ECG:K2_STREAMSHIELD",
        "ECG_K2_STREAMSHIELD",
        "ECG:K2_SS",
        "ECG_K2_SS",
    ):
        return PolicySpec(
            label="ECG_K2_STREAMSHIELD",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_schedule_k=2,
            ecg_stream_bypass=True,
            ecg_variant="adaptive",
            ecg_transport_pinned=True,
        )
    if upper in ("ECG:K2_ONLINE", "ECG_K2_ONLINE"):
        return PolicySpec(
            label="ECG_K2_ONLINE",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_schedule_k=2,
            ecg_variant="rrip_first",
            ecg_transport_pinned=True,
            ecg_set_dueling=True,
        )
    if upper in (
        "ECG:K2_ONLINE_STREAMSHIELD",
        "ECG_K2_ONLINE_STREAMSHIELD",
        "ECG:K2_ONLINE_SS",
        "ECG_K2_ONLINE_SS",
    ):
        return PolicySpec(
            label="ECG_K2_ONLINE_STREAMSHIELD",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_schedule_k=2,
            ecg_stream_bypass=True,
            ecg_variant="rrip_first",
            ecg_transport_pinned=True,
            ecg_set_dueling=True,
        )
    if upper in (
        "ECG:K2_ADAPTIVE_STREAMSHIELD",
        "ECG_K2_ADAPTIVE_STREAMSHIELD",
        "ECG:K2_ADAPTIVE_SS",
        "ECG_K2_ADAPTIVE_SS",
    ):
        return PolicySpec(
            label="ECG_K2_ADAPTIVE_STREAMSHIELD",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_schedule_k=2,
            ecg_stream_bypass=True,
            ecg_stream_adaptive=True,
            ecg_transport_pinned=True,
        )
    if upper in (
        "ECG:K2_ONLINE_ADAPTIVE_STREAMSHIELD",
        "ECG_K2_ONLINE_ADAPTIVE_STREAMSHIELD",
        "ECG:K2_ONLINE_ADAPTIVE_SS",
        "ECG_K2_ONLINE_ADAPTIVE_SS",
    ):
        return PolicySpec(
            label="ECG_K2_ONLINE_ADAPTIVE_STREAMSHIELD",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_schedule_k=2,
            ecg_stream_bypass=True,
            ecg_stream_adaptive=True,
            ecg_variant="rrip_first",
            ecg_transport_pinned=True,
            ecg_set_dueling=True,
        )
    if upper in ("ECG:K1", "ECG_K1"):
        return PolicySpec(
            label="ECG_K1",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_variant="epoch_first",
            ecg_transport_pinned=True,
        )
    if upper in (
        "ECG:K1_STREAMSHIELD",
        "ECG_K1_STREAMSHIELD",
        "ECG:K1_SS",
        "ECG_K1_SS",
    ):
        return PolicySpec(
            label="ECG_K1_STREAMSHIELD",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_stream_bypass=True,
            ecg_variant="epoch_first",
            ecg_transport_pinned=True,
        )
    if upper.startswith("ECG:"):
        mode = upper.split(":", 1)[1]
        label = f"ECG_{mode}" + ("_CHARGED" if charge_popt else "")
        return PolicySpec(
            label=label,
            policy="ECG",
            ecg_mode=mode,
            charge_popt_overhead=charge_popt,
        )
    if upper.startswith("ECG_") and upper != "ECG":
        mode = upper.split("ECG_", 1)[1]
        label = f"ECG_{mode}" + ("_CHARGED" if charge_popt else "")
        return PolicySpec(
            label=label,
            policy="ECG",
            ecg_mode=mode,
            charge_popt_overhead=charge_popt,
        )
    if upper in ("P_OPT", "POPT"):
        if not explicit_charge:
            charge_popt = True
        return PolicySpec(
            label="POPT" if charge_popt else "POPT_UNCHARGED",
            policy="POPT",
            charge_popt_overhead=charge_popt,
        )
    if upper in ("HAWKEYE_PROXY", "HAWKEYE:PROXY"):
        return PolicySpec(
            label="HAWKEYE_PROXY",
            policy="HAWKEYE",
            charge_popt_overhead=False,
        )
    if upper == "HAWKEYE":
        return PolicySpec(
            label="HAWKEYE",
            policy="HAWKEYE",
            charge_popt_overhead=False,
        )
    return PolicySpec(
        label=upper,
        policy=upper,
        charge_popt_overhead=charge_popt,
    )


def policy_output_label(text: str) -> str:
    return parse_policy_spec(text).label
