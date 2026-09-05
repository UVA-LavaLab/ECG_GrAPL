"""Canonical ECG experiment policy parsing and output labels."""

from __future__ import annotations

import re
from dataclasses import dataclass


ONLINE_DUELING_WINDOW_MISSES = 1024
ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS = (
    "gem5_reuse_plan_dueling_request_bound_victims",
    "gem5_reuse_plan_dueling_leader_samples",
    "gem5_reuse_plan_dueling_follower_selections",
    "gem5_reuse_plan_dueling_completed_windows",
)
ONLINE_DUELING_REPORTED_FIELDS = (
    *ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS,
    "gem5_reuse_plan_dueling_winner_changes",
    "gem5_reuse_plan_dueling_follower_variant_overrides",
)

# Sniper analog of the gem5 online-dueling evidence above. Field names use a
# "sniper_" prefix and "governed_victims" rather than gem5's frozen
# "request_bound_victims": Sniper has no O3 Request/MSHR-attested victim to
# bind to, so its population is the closest Sniper-equivalent (a
# marker/sideband-governed miss population; see cache_set_ecg.cc's
# OnlineDuelingEvidence comment and sniper_reuse_bind_dueling_model). The
# frozen gem5_* fields above are never renamed or repurposed for Sniper.
SNIPER_ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS = (
    "sniper_reuse_plan_dueling_governed_victims",
    "sniper_reuse_plan_dueling_leader_samples",
    "sniper_reuse_plan_dueling_follower_selections",
    "sniper_reuse_plan_dueling_completed_windows",
)
SNIPER_ONLINE_DUELING_REPORTED_FIELDS = (
    *SNIPER_ONLINE_DUELING_REQUIRED_POSITIVE_FIELDS,
    "sniper_reuse_plan_dueling_winner_changes",
    "sniper_reuse_plan_dueling_follower_variant_overrides",
)


@dataclass(frozen=True)
class PolicySpec:
    label: str
    policy: str
    ecg_mode: str | None = None
    charge_popt_overhead: bool = False
    ecg_reuse_plan_depth: int = 0
    ecg_flowthrough: bool = False
    ecg_flowthrough_adaptive: bool = False
    ecg_variant: str | None = None
    ecg_transport_pinned: bool = False
    ecg_set_dueling: bool = False
    ecg_reuse_admission: bool = False
    ecg_combined_admission: bool = False
    ecg_online_admission: bool = False
    popt_se_postfinal: str | None = None

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

    if upper in ("ECG:REUSE_PLAN", "ECG_REUSE_PLAN"):
        return PolicySpec(
            label="ECG_REUSE_PLAN",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_variant="adaptive",
            ecg_transport_pinned=True,
        )
    reuse_plan_variants = {
        "ECG:REUSE_PLAN_GRASP": ("ECG_REUSE_PLAN_GRASP", "grasp_only"),
        "ECG_REUSE_PLAN_GRASP": ("ECG_REUSE_PLAN_GRASP", "grasp_only"),
        "ECG:REUSE_PLAN_EPOCH": ("ECG_REUSE_PLAN_EPOCH", "epoch_first"),
        "ECG_REUSE_PLAN_EPOCH": ("ECG_REUSE_PLAN_EPOCH", "epoch_first"),
        "ECG:REUSE_PLAN_RRIP": ("ECG_REUSE_PLAN_RRIP", "rrip_first"),
        "ECG_REUSE_PLAN_RRIP": ("ECG_REUSE_PLAN_RRIP", "rrip_first"),
        "ECG:REUSE_PLAN_DEGREE": ("ECG_REUSE_PLAN_DEGREE", "degree_first"),
        "ECG_REUSE_PLAN_DEGREE": ("ECG_REUSE_PLAN_DEGREE", "degree_first"),
        "ECG:REUSE_PLAN_SHORTCIRCUIT":
            ("ECG_REUSE_PLAN_SHORTCIRCUIT", "shortcircuit"),
        "ECG_REUSE_PLAN_SHORTCIRCUIT":
            ("ECG_REUSE_PLAN_SHORTCIRCUIT", "shortcircuit"),
        "ECG:REUSE_PLAN_LRU": ("ECG_REUSE_PLAN_LRU", "lru_only"),
        "ECG_REUSE_PLAN_LRU": ("ECG_REUSE_PLAN_LRU", "lru_only"),
    }
    if upper in reuse_plan_variants:
        label, variant = reuse_plan_variants[upper]
        return PolicySpec(
            label=label,
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_variant=variant,
            ecg_transport_pinned=True,
        )
    reuse_plan_flowthrough_variants = {
        "ECG:REUSE_PLAN_GRASP_FLOWTHROUGH":
            ("ECG_REUSE_PLAN_GRASP_FLOWTHROUGH", "grasp_only"),
        "ECG_REUSE_PLAN_GRASP_FLOWTHROUGH":
            ("ECG_REUSE_PLAN_GRASP_FLOWTHROUGH", "grasp_only"),
        "ECG:REUSE_PLAN_EPOCH_FLOWTHROUGH":
            ("ECG_REUSE_PLAN_EPOCH_FLOWTHROUGH", "epoch_first"),
        "ECG_REUSE_PLAN_EPOCH_FLOWTHROUGH":
            ("ECG_REUSE_PLAN_EPOCH_FLOWTHROUGH", "epoch_first"),
        "ECG:REUSE_PLAN_RECORD_LRU_FLOWTHROUGH":
            ("ECG_REUSE_PLAN_RECORD_LRU_FLOWTHROUGH", "record_lru"),
        "ECG_REUSE_PLAN_RECORD_LRU_FLOWTHROUGH":
            ("ECG_REUSE_PLAN_RECORD_LRU_FLOWTHROUGH", "record_lru"),
        "ECG:REUSE_PLAN_RRIP_NO_EPOCH_FLOWTHROUGH":
            ("ECG_REUSE_PLAN_RRIP_NO_EPOCH_FLOWTHROUGH", "rrip_no_epoch"),
        "ECG_REUSE_PLAN_RRIP_NO_EPOCH_FLOWTHROUGH":
            ("ECG_REUSE_PLAN_RRIP_NO_EPOCH_FLOWTHROUGH", "rrip_no_epoch"),
        "ECG:REUSE_PLAN_RRIP_NO_EPOCH_RECENCY_FLOWTHROUGH":
            (
                "ECG_REUSE_PLAN_RRIP_NO_EPOCH_RECENCY_FLOWTHROUGH",
                "rrip_no_epoch_recency"),
        "ECG_REUSE_PLAN_RRIP_NO_EPOCH_RECENCY_FLOWTHROUGH":
            (
                "ECG_REUSE_PLAN_RRIP_NO_EPOCH_RECENCY_FLOWTHROUGH",
                "rrip_no_epoch_recency"),
        "ECG:REUSE_PLAN_FUTURE_TIER_FLOWTHROUGH":
            (
                "ECG_REUSE_PLAN_FUTURE_TIER_FLOWTHROUGH",
                "future_tier_first"),
        "ECG_REUSE_PLAN_FUTURE_TIER_FLOWTHROUGH":
            (
                "ECG_REUSE_PLAN_FUTURE_TIER_FLOWTHROUGH",
                "future_tier_first"),
        "ECG:REUSE_PLAN_DEGREE_FLOWTHROUGH":
            ("ECG_REUSE_PLAN_DEGREE_FLOWTHROUGH", "degree_first"),
        "ECG_REUSE_PLAN_DEGREE_FLOWTHROUGH":
            ("ECG_REUSE_PLAN_DEGREE_FLOWTHROUGH", "degree_first"),
        "ECG:REUSE_PLAN_SHORTCIRCUIT_FLOWTHROUGH":
            ("ECG_REUSE_PLAN_SHORTCIRCUIT_FLOWTHROUGH", "shortcircuit"),
        "ECG_REUSE_PLAN_SHORTCIRCUIT_FLOWTHROUGH":
            ("ECG_REUSE_PLAN_SHORTCIRCUIT_FLOWTHROUGH", "shortcircuit"),
    }
    if upper in reuse_plan_flowthrough_variants:
        label, variant = reuse_plan_flowthrough_variants[upper]
        return PolicySpec(
            label=label,
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_flowthrough=True,
            ecg_variant=variant,
            ecg_transport_pinned=True,
        )
    # `epoch_only` remains an internal compatibility spelling for
    # EPOCH_FIRST. Its victim decision does not consume insertion RRPV, so it
    # is not a distinct experimental policy and intentionally has no label.
    if upper in (
        "ECG:REUSE_PLAN_RRIP_FLOWTHROUGH",
        "ECG_REUSE_PLAN_RRIP_FLOWTHROUGH",
    ):
        return PolicySpec(
            label="ECG_REUSE_PLAN_RRIP_FLOWTHROUGH",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_flowthrough=True,
            ecg_variant="rrip_first",
            ecg_transport_pinned=True,
        )
    if upper in (
        "ECG:REUSE_PLAN_ADMISSION_FLOWTHROUGH",
        "ECG_REUSE_PLAN_ADMISSION_FLOWTHROUGH",
    ):
        return PolicySpec(
            label="ECG_REUSE_PLAN_ADMISSION_FLOWTHROUGH",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_flowthrough=True,
            ecg_variant="rrip_first",
            ecg_transport_pinned=True,
            ecg_reuse_admission=True,
        )
    if upper in (
        "ECG:REUSE_PLAN_RECENCY_ADMISSION_FLOWTHROUGH",
        "ECG_REUSE_PLAN_RECENCY_ADMISSION_FLOWTHROUGH",
    ):
        return PolicySpec(
            label="ECG_REUSE_PLAN_RECENCY_ADMISSION_FLOWTHROUGH",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_flowthrough=True,
            ecg_variant="rrip_no_epoch_recency",
            ecg_transport_pinned=True,
            ecg_reuse_admission=True,
        )
    if upper in (
        "ECG:REUSE_PLAN_COMBINED_ADMISSION_FLOWTHROUGH",
        "ECG_REUSE_PLAN_COMBINED_ADMISSION_FLOWTHROUGH",
    ):
        return PolicySpec(
            label="ECG_REUSE_PLAN_COMBINED_ADMISSION_FLOWTHROUGH",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_flowthrough=True,
            ecg_variant="rrip_no_epoch_recency",
            ecg_transport_pinned=True,
            ecg_reuse_admission=True,
            ecg_combined_admission=True,
        )
    if upper in (
        "ECG:REUSE_PLAN_ONLINE_ADMISSION_FLOWTHROUGH",
        "ECG_REUSE_PLAN_ONLINE_ADMISSION_FLOWTHROUGH",
    ):
        return PolicySpec(
            label="ECG_REUSE_PLAN_ONLINE_ADMISSION_FLOWTHROUGH",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_flowthrough=True,
            ecg_variant="rrip_no_epoch_recency",
            ecg_transport_pinned=True,
            ecg_online_admission=True,
        )
    if upper in (
        "ECG:REUSE_PLAN_LRU_FLOWTHROUGH",
        "ECG_REUSE_PLAN_LRU_FLOWTHROUGH",
    ):
        return PolicySpec(
            label="ECG_REUSE_PLAN_LRU_FLOWTHROUGH",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_flowthrough=True,
            ecg_variant="lru_only",
            ecg_transport_pinned=True,
        )
    if upper in (
        "ECG:REUSE_PLAN_FLOWTHROUGH",
        "ECG_REUSE_PLAN_FLOWTHROUGH",
    ):
        return PolicySpec(
            label="ECG_REUSE_PLAN_FLOWTHROUGH",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_flowthrough=True,
            ecg_variant="adaptive",
            ecg_transport_pinned=True,
        )
    if upper in ("ECG:REUSE_PLAN_ONLINE", "ECG_REUSE_PLAN_ONLINE"):
        return PolicySpec(
            label="ECG_REUSE_PLAN_ONLINE",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_variant="rrip_first",
            ecg_transport_pinned=True,
            ecg_set_dueling=True,
        )
    if upper in (
        "ECG:REUSE_PLAN_ONLINE_FLOWTHROUGH",
        "ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH",
    ):
        return PolicySpec(
            label="ECG_REUSE_PLAN_ONLINE_FLOWTHROUGH",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_flowthrough=True,
            ecg_variant="rrip_first",
            ecg_transport_pinned=True,
            ecg_set_dueling=True,
        )
    if upper in (
        "ECG:REUSE_PLAN_ADAPTIVE_FLOWTHROUGH",
        "ECG_REUSE_PLAN_ADAPTIVE_FLOWTHROUGH",
    ):
        return PolicySpec(
            label="ECG_REUSE_PLAN_ADAPTIVE_FLOWTHROUGH",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_flowthrough=True,
            ecg_flowthrough_adaptive=True,
            ecg_transport_pinned=True,
        )
    if upper in ("ECG:NEXT_USE_LRU", "ECG_NEXT_USE_LRU"):
        return PolicySpec(
            label="ECG_NEXT_USE_LRU",
            policy="ECG",
            ecg_mode="ECG_EXACT_STORED",
            ecg_variant="next_use_lru",
        )
    if upper in ("GRASP:PAPER", "GRASP_PAPER"):
        return PolicySpec(
            label="GRASP_PAPER",
            policy="GRASP",
        )
    if upper in (
        "ECG:REF32_EXACT_COMMIT", "ECG_REF32_EXACT_COMMIT",
    ):
        return PolicySpec(
            label="ECG_REF32_EXACT_COMMIT",
            policy="ECG",
            ecg_mode="ECG_REF32",
        )
    if upper in ("ECG:REF32_R_COMMIT", "ECG_REF32_R_COMMIT"):
        return PolicySpec(
            label="ECG_REF32_R_COMMIT",
            policy="ECG",
            ecg_mode="ECG_REF32",
        )
    if upper in ("ECG:REF32_T", "ECG_REF32_T"):
        return PolicySpec(
            label="ECG_REF32_T",
            policy="LRU",
            ecg_mode="ECG_REF32",
        )
    if upper in ("ECG:REF32_P", "ECG_REF32_P"):
        return PolicySpec(
            label="ECG_REF32_P",
            policy="LRU",
            ecg_mode="ECG_REF32",
        )
    if upper in ("ECG:REF32_RP_COMMIT", "ECG_REF32_RP_COMMIT"):
        return PolicySpec(
            label="ECG_REF32_RP_COMMIT",
            policy="ECG",
            ecg_mode="ECG_REF32",
        )
    if upper in (
        "ECG:REF32_SCALE_R_COMMIT", "ECG_REF32_SCALE_R_COMMIT",
    ):
        return PolicySpec(
            label="ECG_REF32_SCALE_R_COMMIT",
            policy="ECG",
            ecg_mode="ECG_REF32",
        )
    if upper in (
        "ECG:REF32_SCALE_RP_COMMIT", "ECG_REF32_SCALE_RP_COMMIT",
    ):
        return PolicySpec(
            label="ECG_REF32_SCALE_RP_COMMIT",
            policy="ECG",
            ecg_mode="ECG_REF32",
        )
    if upper in (
        "ECG:REUSE_PLAN_ONLINE_ADAPTIVE_FLOWTHROUGH",
        "ECG_REUSE_PLAN_ONLINE_ADAPTIVE_FLOWTHROUGH",
    ):
        return PolicySpec(
            label="ECG_REUSE_PLAN_ONLINE_ADAPTIVE_FLOWTHROUGH",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_reuse_plan_depth=2,
            ecg_flowthrough=True,
            ecg_flowthrough_adaptive=True,
            ecg_variant="rrip_first",
            ecg_transport_pinned=True,
            ecg_set_dueling=True,
        )
    if upper in ("ECG:REUSE_PLAN_1", "ECG_REUSE_PLAN_1"):
        return PolicySpec(
            label="ECG_REUSE_PLAN_1",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_variant="epoch_first",
            ecg_transport_pinned=True,
        )
    if upper in (
        "ECG:REUSE_PLAN_1_FLOWTHROUGH",
        "ECG_REUSE_PLAN_1_FLOWTHROUGH",
    ):
        return PolicySpec(
            label="ECG_REUSE_PLAN_1_FLOWTHROUGH",
            policy="ECG",
            ecg_mode="ECG_GRASP_POPT",
            ecg_flowthrough=True,
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
    if upper in ("POPT_SE", "POPT:SE", "P_OPT_SE",
                 "POPT_SE_DISTANT", "POPT:SE_DISTANT", "P_OPT_SE_DISTANT"):
        if not explicit_charge:
            charge_popt = True
        distant = upper.endswith("_DISTANT")
        label = "POPT_SE_DISTANT" if distant else "POPT_SE"
        return PolicySpec(
            label=label if charge_popt else f"{label}_UNCHARGED",
            policy="POPT",
            charge_popt_overhead=charge_popt,
            popt_se_postfinal="distant" if distant else "later_lower_bound",
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
