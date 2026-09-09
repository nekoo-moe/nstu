#include "nstu/setup/diagnostics.hpp"

#include <cassert>
#include <cstdint>

int main() {
    assert(nstu::setup::is_uwf_supported_product(0x79));
    assert(nstu::setup::is_uwf_supported_product(0xbc));
    assert(!nstu::setup::is_uwf_supported_product(0x30));

    nstu::setup::UwfProbeSnapshot unsupported{};
    unsupported.product_type = 0x30;
    assert(nstu::setup::classify_uwf(unsupported) ==
           nstu::setup::UwfState::unsupported_edition);

    nstu::setup::UwfProbeSnapshot missing{};
    missing.product_type = 0x79;
    missing.feature_known = true;
    assert(nstu::setup::classify_uwf(missing) ==
           nstu::setup::UwfState::feature_missing);

    nstu::setup::UwfProbeSnapshot unavailable{};
    unavailable.product_type = 0x79;
    unavailable.feature_known = true;
    unavailable.feature_enabled = true;
    assert(nstu::setup::classify_uwf(unavailable) ==
           nstu::setup::UwfState::provider_unavailable);

    nstu::setup::UwfProbeSnapshot pending = unavailable;
    pending.provider_available = true;
    pending.filter_state_known = true;
    pending.current_enabled = false;
    pending.next_enabled = true;
    assert(nstu::setup::classify_uwf(pending) ==
           nstu::setup::UwfState::reboot_pending);

    nstu::setup::UwfProbeSnapshot enabled = pending;
    enabled.current_enabled = true;
    assert(nstu::setup::classify_uwf(enabled) ==
           nstu::setup::UwfState::enabled);

    assert(nstu::setup::classify_memory_gib(7) ==
           nstu::setup::Readiness::minimum_not_met);
    assert(nstu::setup::classify_memory_gib(8) ==
           nstu::setup::Readiness::good);
    assert(nstu::setup::classify_link_speed_mbps(99) ==
           nstu::setup::Readiness::minimum_not_met);
    assert(nstu::setup::classify_link_speed_mbps(100) ==
           nstu::setup::Readiness::recommended_not_met);
    assert(nstu::setup::classify_link_speed_mbps(1000) ==
           nstu::setup::Readiness::good);
    assert(nstu::setup::classify_processor(false, 8, 8) ==
           nstu::setup::Readiness::minimum_not_met);
    assert(nstu::setup::classify_processor(true, 4, 4) ==
           nstu::setup::Readiness::good);
    assert(nstu::setup::classify_processor(true, 2, 4) ==
           nstu::setup::Readiness::minimum_not_met);

    const nstu::setup::DiagnosticOptions options{};
    const auto checks = nstu::setup::diagnostic_checks(options);
    assert(!checks.empty());
    assert(checks.front().id == "os");
    return 0;
}
