/*
 * SPDX-FileCopyrightText: 2025-2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "OPlusSensorPropsShim"

#include <aidl/android/hardware/biometrics/common/SensorStrength.h>
#include <aidl/android/hardware/biometrics/fingerprint/FingerprintSensorType.h>
#include <aidl/android/hardware/biometrics/fingerprint/SensorLocation.h>
#include <aidl/android/hardware/biometrics/fingerprint/SensorProps.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android/binder_auto_utils.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>

#include <cstdint>
#include <dlfcn.h>
#include <optional>
#include <string>
#include <utility>

using android::base::GetIntProperty;
using android::base::GetProperty;
using android::base::ParseInt;
using android::base::ReadFileToString;
using android::base::Split;
using android::base::Tokenize;
using android::base::Trim;

using aidl::android::hardware::biometrics::common::SensorStrength;
using aidl::android::hardware::biometrics::fingerprint::FingerprintSensorType;
using aidl::android::hardware::biometrics::fingerprint::SensorLocation;
using aidl::android::hardware::biometrics::fingerprint::SensorProps;

/*
 * Force-loaded into the stock OPlus fingerprint HAL. That HAL is built against
 * an old frozen copy of the AIDL fingerprint interface; this shim is built
 * against the current one. Three separate incompatibilities to handle:
 *
 * (A) SensorProps wire layout -- SensorProps::writeToParcel
 *     The framework parses the bytes with the layout from *its* headers. The
 *     version reported below does not affect that. So the HAL has to emit
 *     current-layout bytes.
 *       - value-copying *thisptr (upstream's original code) reads a struct laid
 *         out by the HAL's older headers, hits garbage in
 *         commonProps.componentInfo and dies in std::__throw_length_error under
 *         -fno-exceptions -> SIGABRT;
 *       - passing *thisptr straight through emits old-layout bytes ->
 *         IllegalArgumentException inside
 *         FingerprintSensorConfigurations.getSensorPropForInstance on the
 *         BiometricServiceRegistry thread -> system_server boot loop;
 *       - handing a freshly built V2 object to the V2 serialiser and then
 *         appending the trailing field the current definition added, while
 *         fixing the block length prefix, is what actually works.
 *     Return type MUST be binder_status_t: it comes back in w0 and the HAL
 *     treats a non-zero value as failure (PruneStatusT: Unknown status_t).
 *
 * (B) Interface version -- BnFingerprint/BnSession getInterfaceVersion+Hash
 *     Upstream never overrode these, so the HAL advertises the version of the
 *     NdK it links (2). The framework reads it from IFingerprint
 *     (FingerprintStartUserClient) and from ISession, deriving
 *     hasContextMethods() from it, then calls enrollWithContext /
 *     authenticateWithContext, which this HAL does not implement -> the session
 *     dies with DeadObjectException. Report 1 and drop the hash.
 *     These must return ndk::ScopedAStatus BY VALUE: it is a struct return, so
 *     AAPCS64 passes its address in x8. Modelling it as a status out-parameter
 *     puts the pointer in x2 where the caller put nothing, and the store faults
 *     (observed: SIGSEGV at 0x5).
 */

namespace {

std::optional<std::pair<int32_t, int32_t>> GetScreenResolution() {
    std::string value;
    if (!ReadFileToString("/sys/class/drm/card0-DSI-1/modes", &value)) {
        return {};
    }

    auto lines = Split(Trim(value), "\n");
    if (lines.empty()) {
        return {};
    }

    auto parts = Split(lines.back(), "x");
    if (parts.size() < 2) {
        return {};
    }

    int32_t width, height;
    if (!ParseInt(parts[0], &width) || !ParseInt(parts[1], &height)) {
        return {};
    }

    return std::make_pair(width, height);
}

void SensorPropsInit(SensorProps* props) {
    auto type = GetProperty("persist.vendor.fingerprint.sensor_type", "");
    if (!type.empty()) {
        if (type == "back")
            props->sensorType = FingerprintSensorType::REAR;
        else if (type == "ultrasonic")
            props->sensorType = FingerprintSensorType::UNDER_DISPLAY_ULTRASONIC;
        else if (type == "optical")
            props->sensorType = FingerprintSensorType::UNDER_DISPLAY_OPTICAL;
        else if (type == "side")
            props->sensorType = FingerprintSensorType::POWER_BUTTON;
        else if (type == "front")
            props->sensorType = FingerprintSensorType::HOME_BUTTON;
    }

    auto icon_loc_prop = GetIntProperty("persist.vendor.fingerprint.optical.iconlocation", 0);
    auto loc_prop = GetProperty("persist.vendor.fingerprint.optical.sensorlocation",
                                GetProperty("persist.vendor.fingerprint.side.sensorlocation", ""));
    auto screen_resolution = GetScreenResolution();
    if (icon_loc_prop > 0 && screen_resolution &&
        (props->sensorType == FingerprintSensorType::UNDER_DISPLAY_ULTRASONIC ||
         props->sensorType == FingerprintSensorType::UNDER_DISPLAY_OPTICAL)) {
        SensorLocation loc;
        loc.sensorLocationX = screen_resolution->first / 2;
        loc.sensorLocationY = screen_resolution->second - icon_loc_prop;
        props->sensorLocations = {loc};
    } else if (!loc_prop.empty()) {
        auto locations = Tokenize(loc_prop, "|");
        props->sensorLocations.clear();

        for (const auto& location : locations) {
            auto parts = Tokenize(location, "::");
            bool loc_parsed = false;
            if (parts.size() >= 2) {
                int32_t x, y;
                loc_parsed = ParseInt(parts[0], &x) && ParseInt(parts[1], &y);
                if (loc_parsed) {
                    SensorLocation loc;
                    loc.sensorLocationX = x;
                    loc.sensorLocationY = y;
                    if (parts.size() >= 3 && !parts[2].empty()) {
                        loc.display = "local:" + parts[2];
                    }
                    props->sensorLocations.push_back(loc);
                }
            }
            LOG_IF(WARNING, !loc_parsed)
                    << "Invalid sensor location input (x::y[::display]): " << location;
        }
    }

    auto size = GetProperty("persist.vendor.fingerprint.optical.iconsize", "");
    if (!size.empty() && !props->sensorLocations.empty()) {
        if (ParseInt(size, &props->sensorLocations[0].sensorRadius)) {
            props->sensorLocations[0].sensorRadius /= 2;
        } else {
            LOG(WARNING) << "Invalid sensor size input: " << size;
        }
    }

    props->halHandlesDisplayTouches =
            props->sensorType == FingerprintSensorType::UNDER_DISPLAY_OPTICAL ||
            props->sensorType == FingerprintSensorType::UNDER_DISPLAY_ULTRASONIC;
}

}  // anonymous namespace

extern "C" binder_status_t
_ZNK4aidl7android8hardware10biometrics11fingerprint11SensorProps13writeToParcelEP7AParcel(
        SensorProps* /*thisptr*/, AParcel* parcel) {
    static auto v2Write =
            reinterpret_cast<binder_status_t (*)(const SensorProps*, AParcel*)>(
                    dlsym(RTLD_NEXT, __func__));

    SensorProps props;
    props.commonProps.sensorId = 0;
    props.commonProps.sensorStrength = SensorStrength::STRONG;
    props.commonProps.maxEnrollmentsPerUser = 5;
    props.supportsNavigationGestures = false;
    props.supportsDetectInteraction = true;
    props.halControlsIllumination = true;

    SensorPropsInit(&props);

    LOG(INFO) << "emitting synthesised SensorProps: " << props.toString();

    int32_t start = AParcel_getDataPosition(parcel);

    binder_status_t status = v2Write(&props, parcel);
    if (status != STATUS_OK) {
        LOG(ERROR) << "SensorProps base serialisation failed: " << status;
        return status;
    }

    status = AParcel_writeBool(parcel, false);
    if (status != STATUS_OK) return status;

    int32_t finish = AParcel_getDataPosition(parcel);
    status = AParcel_setDataPosition(parcel, start);
    if (status != STATUS_OK) return status;
    status = AParcel_writeInt32(parcel, finish - start);
    if (status != STATUS_OK) return status;
    return AParcel_setDataPosition(parcel, finish);
}

// C++ linkage (so returning ScopedAStatus is not a C-linkage violation), but
// the emitted symbol name is pinned with asm() to the mangled name the HAL's
// virtual dispatch will look up. Defining these as extern "C" instead trips
// -Wreturn-type-c-linkage: ScopedAStatus is a C++ struct.
ndk::ScopedAStatus
opGetFingerprintInterfaceVersion(void* /*thisptr*/, int32_t* version)
        asm("_ZN4aidl7android8hardware10biometrics11fingerprint13BnFingerprint19getInterfaceVersionEPi");

ndk::ScopedAStatus
opGetFingerprintInterfaceVersion(void* /*thisptr*/, int32_t* version) {
    if (version != nullptr) {
        *version = 1;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus
opGetFingerprintInterfaceHash(void* /*thisptr*/, std::string* /*hash*/)
        asm("_ZN4aidl7android8hardware10biometrics11fingerprint13BnFingerprint16getInterfaceHashEPNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEE");

ndk::ScopedAStatus
opGetFingerprintInterfaceHash(void* /*thisptr*/, std::string* /*hash*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus
opGetSessionInterfaceVersion(void* /*thisptr*/, int32_t* version)
        asm("_ZN4aidl7android8hardware10biometrics11fingerprint9BnSession19getInterfaceVersionEPi");

ndk::ScopedAStatus
opGetSessionInterfaceVersion(void* /*thisptr*/, int32_t* version) {
    if (version != nullptr) {
        *version = 1;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus
opGetSessionInterfaceHash(void* /*thisptr*/, std::string* /*hash*/)
        asm("_ZN4aidl7android8hardware10biometrics11fingerprint9BnSession16getInterfaceHashEPNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEE");

ndk::ScopedAStatus
opGetSessionInterfaceHash(void* /*thisptr*/, std::string* /*hash*/) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
