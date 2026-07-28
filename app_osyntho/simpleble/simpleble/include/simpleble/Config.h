#pragma once
#include <chrono>

namespace SimpleBLE {
namespace Config {
    namespace SimpleBluez {
        extern bool use_legacy_bluez_backend;
        extern std::chrono::steady_clock::duration connection_timeout;
        extern std::chrono::steady_clock::duration disconnection_timeout;

        static void reset() {
            use_legacy_bluez_backend = true;
            connection_timeout = std::chrono::seconds(2);
            disconnection_timeout = std::chrono::seconds(1);
        }
    }

    namespace WinRT {
        extern bool experimental_use_own_mta_apartment;
        extern bool experimental_reinitialize_winrt_apartment_on_main_thread;

        static void reset() {
            experimental_use_own_mta_apartment = true;
            experimental_reinitialize_winrt_apartment_on_main_thread = false;
        }
    }

    namespace CoreBluetooth {
        static void reset() {}
    }

    namespace Android {
        enum class ConnectionPriorityRequest {
            DISABLED = -1,
            BALANCED = 0,
            HIGH = 1,
            LOW_POWER = 2,
            DCK = 3
        };

        extern ConnectionPriorityRequest connection_priority_request;

        static void reset() {
            connection_priority_request = ConnectionPriorityRequest::DISABLED;
        }
    }

    namespace Dongl {
        extern bool use_dongl_backend;
        static void reset() {
            use_dongl_backend = false;
        }
    }

    namespace Base {
        static void reset_all() {
            SimpleBluez::reset();
            WinRT::reset();
            CoreBluetooth::reset();
            Android::reset();
        }
    }
}  // namespace Config
}  // namespace SimpleBLE