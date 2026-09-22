#include "nstu/setup/uwf.hpp"

#include <cassert>

int main() {
    using nstu::setup::UwfEnableGate;
    using nstu::setup::UwfEnableReadiness;

    UwfEnableReadiness readiness;
    assert(nstu::setup::evaluate_uwf_enable(readiness) ==
           UwfEnableGate::probe_unavailable);

    readiness.product_type = 0x30; // Windows Pro
    readiness.feature_known = true;
    readiness.feature_enabled = true;
    assert(nstu::setup::evaluate_uwf_enable(readiness) ==
           UwfEnableGate::unsupported_edition);

    readiness.product_type = 0x79; // Windows Education
    readiness.feature_enabled = false;
    assert(nstu::setup::evaluate_uwf_enable(readiness) ==
           UwfEnableGate::feature_missing);

    readiness.feature_enabled = true;
    assert(nstu::setup::evaluate_uwf_enable(readiness) ==
           UwfEnableGate::provider_unavailable);

    readiness.provider_available = true;
    readiness.filter_state_known = true;
    readiness.next_enabled = true;
    assert(nstu::setup::evaluate_uwf_enable(readiness) ==
           UwfEnableGate::reboot_pending);

    readiness.current_enabled = true;
    assert(nstu::setup::evaluate_uwf_enable(readiness) ==
           UwfEnableGate::invalid_data_root);

    readiness.current_enabled = false;
    readiness.next_enabled = false;
    assert(nstu::setup::evaluate_uwf_enable(readiness) ==
           UwfEnableGate::invalid_data_root);

    readiness.data_root_valid = true;
    assert(nstu::setup::evaluate_uwf_enable(readiness) ==
           UwfEnableGate::readiness_failed);

    readiness.diagnostic_readiness_passed = true;
    assert(nstu::setup::evaluate_uwf_enable(readiness) ==
           UwfEnableGate::checkpoint_required);

    readiness.checkpoint_acknowledged = true;
    assert(nstu::setup::evaluate_uwf_enable(readiness) ==
           UwfEnableGate::ready);
    readiness.current_enabled = true;
    readiness.next_enabled = true;
    assert(nstu::setup::evaluate_uwf_enable(readiness) ==
           UwfEnableGate::already_enabled);

    // Enterprise and Education variants accepted; Pro refused.
    assert(nstu::setup::is_uwf_supported_product(0x04));
    assert(nstu::setup::is_uwf_supported_product(0x79));
    assert(nstu::setup::is_uwf_supported_product(0xbc));
    assert(!nstu::setup::is_uwf_supported_product(0x30));
    return 0;
}
