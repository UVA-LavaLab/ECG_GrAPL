from scripts.experiments.ecg.verify import ecg


def trace(policy: str, victim: int) -> str:
    return "\n".join([
        f"[EVICT L3 pol={policy} curEpoch=0 set_ways=2]",
        "way0 valid=1 rrpv=7 epoch=1 dist=1 prop=1 stamped=1 "
        "dbg=1 last=5 epoch2=2 sched_n=2",
        "way1 valid=1 rrpv=7 epoch=1 dist=1 prop=1 stamped=1 "
        "dbg=1 last=5 epoch2=2 sched_n=2",
        f"   -> victim=way{victim} reason=test",
    ])


def test_tied_rrip_requires_first_way():
    assert ecg.verify_trace(
        "strict", (trace("ECG:rrip_first", 0), True),
        expected_policy="ECG:rrip_first")
    assert not ecg.verify_trace(
        "strict", (trace("ECG:rrip_first", 1), True),
        expected_policy="ECG:rrip_first")


def test_expected_policy_is_required():
    assert not ecg.verify_trace(
        "strict", (trace("ECG:epoch_first", 0), True),
        expected_policy="ECG:rrip_first")


def gem5_delivery(
        fill_offset: int = 0, source: str = "request",
        reverse_accepts: bool = False) -> str:
    lines = [trace("ECG:rrip_first", 0)]
    for sequence in range(32):
        dest = sequence + 1
        lines.extend([
            f"[ECG-K2-EXPECT sim=gem5 seq={sequence} dest={dest} "
            "tier=1 epoch1=2 epoch2=3 width=4]",
            f"[ECG-K2-RECV sim=gem5 seq={sequence} dest={dest} "
            "tier=1 epoch1=2 epoch2=3 width=4]",
        ])
        if source == "request":
            lines.append(
                f"[ECG-K2-REQUEST sim=gem5 seq={sequence} "
                f"request_seq={100 + sequence} dest={dest} tier=1 "
                "epoch1=2 epoch2=3 current=1 context=7]")
    accept_order = list(range(31, -1, -1)) if reverse_accepts else list(range(32))
    for request_index in accept_order:
        dest = request_index + 1
        lines.append(
            f"[ECG-K2-ACCEPT sim=gem5 seq={request_index} "
            f"request_seq={100 + request_index if source == 'request' else request_index} "
            f"request_dest={dest} "
            f"fill_dest={dest + fill_offset} source=request tier=1 "
            "epoch1=2 epoch2=3 current=1 context=7 width=4]".replace(
                "source=request", f"source={source}"))
    return "\n".join(lines)


def test_gem5_acceptance_must_match_delivery():
    assert ecg.verify_k2_trace(
        "gem5/test", (gem5_delivery(), True), ne=32768,
        expected_policy="ECG:rrip_first")
    assert not ecg.verify_k2_trace(
        "gem5/test", (gem5_delivery(fill_offset=16), True), ne=32768,
        expected_policy="ECG:rrip_first")


def test_gem5_request_acceptance_is_order_independent_but_mailbox_is_serial():
    assert ecg.verify_k2_trace(
        "gem5/test",
        (gem5_delivery(source="request", reverse_accepts=True), True),
        ne=32768, expected_policy="ECG:rrip_first",
        require_request_bound=True)
    assert ecg.verify_k2_trace(
        "gem5/test", (gem5_delivery(source="mailbox"), True), ne=32768,
        expected_policy="ECG:rrip_first")
    assert not ecg.verify_k2_trace(
        "gem5/test", (gem5_delivery(source="mailbox"), True), ne=32768,
        expected_policy="ECG:rrip_first", require_request_bound=True)


def test_o3_request_accept_verifier_uses_request_sequence_not_execute_order():
    text = gem5_delivery(source="request", reverse_accepts=True)
    cov = {}
    assert ecg.verify_k2_request_accepts(
        "gem5/o3", text, expected_width=4, coverage=cov)
    assert cov["k2_o3_request_accept_mismatches"] == 0

    wrong_fill = text.replace("fill_dest=1", "fill_dest=999", 1)
    assert not ecg.verify_k2_request_accepts(
        "gem5/o3", wrong_fill, expected_width=4)


def test_gem5_accepts_only_the_traced_loads_that_reach_llc():
    lines = gem5_delivery(source="mailbox").splitlines()
    accepted_subset = "\n".join(
        line for line in lines
        if "ECG-K2-ACCEPT" not in line or
        any(f"seq={sequence} " in line for sequence in (0, 14, 23)))
    assert ecg.verify_k2_trace(
        "gem5/test", (accepted_subset, True), ne=32768,
        expected_policy="ECG:rrip_first")


def test_duplicate_delivery_sequence_fails():
    text = gem5_delivery()
    duplicate = (
        "[ECG-K2-EXPECT sim=gem5 seq=0 dest=999 tier=1 "
        "epoch1=2 epoch2=3 width=4]\n")
    assert not ecg.verify_k2_trace(
        "gem5/test", (duplicate + text, True), ne=32768,
        expected_policy="ECG:rrip_first",
        require_request_bound=True)


def test_gem5_corrupt_unaccepted_request_fails():
    """A REQUEST whose payload disagrees with EXPECT must fail even when that
    sequence never produced an ACCEPT (i.e. it hit in an inner cache)."""
    lines = gem5_delivery(source="request").splitlines()
    kept = []
    for line in lines:
        if "ECG-K2-ACCEPT" in line and "seq=1 " in line:
            continue
        if "ECG-K2-REQUEST" in line and "seq=1 " in line:
            line = line.replace("dest=2", "dest=999")
        kept.append(line)
    assert not ecg.verify_k2_trace(
        "gem5/test", ("\n".join(kept), True), ne=32768,
        expected_policy="ECG:rrip_first", require_request_bound=True)


def test_gem5_request_subset_without_corruption_still_passes():
    """Positive control for the test above: dropping the ACCEPT alone (a
    legitimate inner-cache hit) must still pass, so the failure there is
    attributable to the corrupt REQUEST payload and not the missing ACCEPT."""
    lines = gem5_delivery(source="request").splitlines()
    kept = [
        line for line in lines
        if not ("ECG-K2-ACCEPT" in line and "seq=1 " in line)
    ]
    assert ecg.verify_k2_trace(
        "gem5/test", ("\n".join(kept), True), ne=32768,
        expected_policy="ECG:rrip_first", require_request_bound=True)
