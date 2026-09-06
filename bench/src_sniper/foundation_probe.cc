#include <cstdint>
#include <cstdio>

#include "sniper_sim/sniper_harness.h"

alignas(64) static volatile uint32_t record4 = 0x89abcdefU;
alignas(64) static volatile uint64_t record8 = 0xf123456789abcdefULL;

int main()
{
    constexpr uint64_t sentinel = 0xfedcba9876543210ULL;

    graphbrew_sniper::roi_begin();
    const uint64_t echoed = graphbrew_sniper::foundation_echo(sentinel);
    graphbrew_sniper::foundation_arm_read(
        &record4, static_cast<uint32_t>(record4), 4);
    const uint32_t loaded4 = record4;
    graphbrew_sniper::foundation_report_loaded(&record4, loaded4, 4);
    graphbrew_sniper::foundation_arm_read(&record8, record8, 8);
    const uint64_t loaded8 = record8;
    graphbrew_sniper::foundation_report_loaded(&record8, loaded8, 8);
    const uint64_t read_status =
        graphbrew_sniper::foundation_read_status();
    graphbrew_sniper::roi_end();

    const bool loaded_protocol = (read_status & 0xc) == 0xc;
    const bool live_response =
        echoed == sentinel && loaded_protocol;
    const bool values_ok =
        loaded4 == 0x89abcdefU &&
        loaded8 == 0xf123456789abcdefULL;
    std::printf(
        "[SNIPER-FOUNDATION-GUEST sentinel=0x%016llx "
        "echoed=0x%016llx read_status=0x%016llx "
        "values=%u loaded_protocol=%u live_response=%u]\n",
        static_cast<unsigned long long>(sentinel),
        static_cast<unsigned long long>(echoed),
        static_cast<unsigned long long>(read_status),
        values_ok ? 1u : 0u, loaded_protocol ? 1u : 0u,
        live_response ? 1u : 0u);
    return values_ok && (echoed == sentinel || echoed == 5) ? 0 : 1;
}
