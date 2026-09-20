// Managed mode, both halves: the one-byte wire form the server and client use
// to agree on it, and the registry flag the client service actually enforces.
// The flag round trip runs against the per-user hive so an unelevated test
// runner proves the same code path the service uses under HKLM.
#include "nstu/client_freeze.hpp"
#include "nstu/client_uwf_request.hpp"
#include "nstu/control_messages.hpp"

#include <windows.h>

#include <cassert>
#include <string>
#include <vector>

namespace {

nstu::client::FreezeLocation test_location() {
    nstu::client::FreezeLocation location;
    location.per_user = true;
    location.subkey = L"Software\\NSTU-Test\\freeze-" +
        std::to_wstring(GetCurrentProcessId());
    return location;
}

void remove_test_key(const nstu::client::FreezeLocation& location) {
    RegDeleteKeyExW(HKEY_CURRENT_USER, location.subkey.c_str(),
                    KEY_WOW64_64KEY, 0);
    RegDeleteKeyExW(HKEY_CURRENT_USER, L"Software\\NSTU-Test", KEY_WOW64_64KEY,
                    0);
}

} // namespace

int main() {
    // One byte, two meanings, and nothing else accepted. A third value is a
    // sender that disagrees with us about the message, not a third state.
    assert(nstu::control::encode_freeze_state(true).size() == 1);
    assert(nstu::control::decode_freeze_state(
               nstu::control::encode_freeze_state(true)) == true);
    assert(nstu::control::decode_freeze_state(
               nstu::control::encode_freeze_state(false)) == false);
    assert(!nstu::control::decode_freeze_state({}).has_value());
    const std::vector<std::byte> two_bytes{std::byte{1}, std::byte{0}};
    assert(!nstu::control::decode_freeze_state(two_bytes).has_value());
    const std::vector<std::byte> out_of_range{std::byte{2}};
    assert(!nstu::control::decode_freeze_state(out_of_range).has_value());
    const std::vector<std::byte> sign_bit{std::byte{0xff}};
    assert(!nstu::control::decode_freeze_state(sign_bit).has_value());

    const auto location = test_location();
    remove_test_key(location);

    // A machine with no flag written is not managed. Freezing has to be an
    // act someone took, never a default a missing key falls into.
    assert(!nstu::client::machine_frozen(location));

    std::string error;
    assert(nstu::client::set_machine_frozen(true, location, &error));
    assert(nstu::client::machine_frozen(location));
    assert(nstu::client::set_machine_frozen(false, location, &error));
    assert(!nstu::client::machine_frozen(location));

    // Wrong type where the flag should be: the service reads that as not
    // managed rather than as managed, so a corrupted value cannot lock a
    // machine that was never frozen.
    assert(nstu::client::set_machine_frozen(true, location, &error));
    {
        HKEY key = nullptr;
        assert(RegOpenKeyExW(HKEY_CURRENT_USER, location.subkey.c_str(), 0,
                             KEY_SET_VALUE | KEY_WOW64_64KEY,
                             &key) == ERROR_SUCCESS);
        const wchar_t text[] = L"1";
        assert(RegSetValueExW(key, location.value.c_str(), 0, REG_SZ,
                              reinterpret_cast<const BYTE*>(text),
                              sizeof(text)) == ERROR_SUCCESS);
        RegCloseKey(key);
    }
    assert(!nstu::client::machine_frozen(location));

    // A subkey that was never created reads the same way, which is what a
    // machine looks like before the installer has ever run.
    nstu::client::FreezeLocation absent = location;
    absent.subkey += L"\\missing";
    assert(!nstu::client::machine_frozen(absent));

    nstu::client::UwfRequestLocation uwf_location;
    uwf_location.per_user = true;
    uwf_location.subkey = location.subkey;
    assert(!nstu::client::uwf_configuration_requested(uwf_location));
    assert(nstu::client::set_uwf_configuration_requested(
        true, uwf_location, &error));
    assert(nstu::client::uwf_configuration_requested(uwf_location));
    assert(nstu::client::set_uwf_configuration_requested(
        false, uwf_location, &error));
    assert(!nstu::client::uwf_configuration_requested(uwf_location));
    {
        HKEY key = nullptr;
        assert(RegOpenKeyExW(HKEY_CURRENT_USER, uwf_location.subkey.c_str(), 0,
                             KEY_SET_VALUE | KEY_WOW64_64KEY,
                             &key) == ERROR_SUCCESS);
        const DWORD malformed_request = 2;
        assert(RegSetValueExW(
                   key, uwf_location.value.c_str(), 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&malformed_request),
                   sizeof(malformed_request)) == ERROR_SUCCESS);
        RegCloseKey(key);
    }
    assert(!nstu::client::uwf_configuration_requested(uwf_location));

    remove_test_key(location);
    assert(!nstu::client::machine_frozen(location));
    return 0;
}
