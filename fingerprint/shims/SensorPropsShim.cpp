/*
 * SPDX-FileCopyrightText: 2025-2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "OPlusSensorPropsShim"

#include <aidl/android/hardware/biometrics/fingerprint/SensorProps.h>

#include <android/binder_status.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>

#include <dlfcn.h>

using android::base::GetIntProperty;
using android::base::GetProperty;
using android::base::ParseInt;
using android::base::ReadFileToString;
using android::base::Split;
using android::base::Tokenize;
using android::base::Trim;

using aidl::android::hardware::biometrics::fingerprint::FingerprintSensorType;
using aidl::android::hardware::biometrics::fingerprint::SensorLocation;
using aidl::android::hardware::biometrics::fingerprint::SensorProps;

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

SensorProps SensorPropsInit(SensorProps props) {
    auto type = GetProperty("persist.vendor.fingerprint.sensor_type", "");
    if (!type.empty()) {
        if (type == "back")
            props.sensorType = FingerprintSensorType::REAR;
        else if (type == "ultrasonic")
            props.sensorType = FingerprintSensorType::UNDER_DISPLAY_ULTRASONIC;
        else if (type == "optical")
            props.sensorType = FingerprintSensorType::UNDER_DISPLAY_OPTICAL;
        else if (type == "side")
            props.sensorType = FingerprintSensorType::POWER_BUTTON;
        else if (type == "front")
            props.sensorType = FingerprintSensorType::HOME_BUTTON;
    }

    auto icon_loc_prop = GetIntProperty("persist.vendor.fingerprint.optical.iconlocation", 0);
    auto loc_prop = GetProperty("persist.vendor.fingerprint.optical.sensorlocation",
                                GetProperty("persist.vendor.fingerprint.side.sensorlocation", ""));
    auto screen_resolution = GetScreenResolution();
    if (icon_loc_prop > 0 && screen_resolution &&
        (props.sensorType == FingerprintSensorType::UNDER_DISPLAY_ULTRASONIC ||
         props.sensorType == FingerprintSensorType::UNDER_DISPLAY_OPTICAL)) {
        SensorLocation loc;
        loc.sensorLocationX = screen_resolution->first / 2;
        loc.sensorLocationY = screen_resolution->second - icon_loc_prop;
        props.sensorLocations = {loc};
    } else if (!loc_prop.empty()) {
        auto locations = Tokenize(loc_prop, "|");
        props.sensorLocations.clear();

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

                    props.sensorLocations.push_back(loc);
                }
            }
            LOG_IF(WARNING, !loc_parsed)
                    << "Invalid sensor location input (x::y[::display]): " << location;
        }
    }

    auto size = GetProperty("persist.vendor.fingerprint.optical.iconsize", "");
    if (!size.empty() && !props.sensorLocations.empty()) {
        if (ParseInt(size, &props.sensorLocations[0].sensorRadius)) {
            props.sensorLocations[0].sensorRadius /= 2;
        } else {
            LOG(WARNING) << "Invalid sensor size input: " << size;
        }
    }

    props.halHandlesDisplayTouches =
            props.sensorType == FingerprintSensorType::UNDER_DISPLAY_OPTICAL ||
            props.sensorType == FingerprintSensorType::UNDER_DISPLAY_ULTRASONIC;

    return props;
}
}  // anonymous namespace

extern "C" binder_status_t
_ZNK4aidl7android8hardware10biometrics11fingerprint11SensorProps13writeToParcelEP7AParcel(
        SensorProps* thisptr, AParcel* parcel) {

    static auto writeToParcel = reinterpret_cast<
            typeof(_ZNK4aidl7android8hardware10biometrics11fingerprint11SensorProps13writeToParcelEP7AParcel)*>(
            dlsym(RTLD_NEXT, __func__));

    if (writeToParcel == nullptr) {
        LOG(ERROR) << "SHIMFIX: dlsym(RTLD_NEXT) 拿不到原始 writeToParcel，放弃改写";
        return STATUS_UNKNOWN_ERROR;
    }

    // ！！绝对不能整体拷贝 thisptr ！！
    // 实测（2026-10-09，ovaltine / salami 移植 blob）：HAL 传进来的对象里
    //   commonProps  读得出正常值（sensorId=1 strength=2 maxEnroll=5 componentInfo=2 项）
    //   sensorLocations 却是未初始化内存（size 读到 1.1e19，0x99 填充）
    // 整体拷贝会在 vector 拷贝里 __throw_length_error -> 无异常模式下直接 abort，HAL 当场死。
    // 所以只读已证实安全的 commonProps，其余字段全部按系统属性重算。
    SensorProps props;
    props.commonProps = thisptr->commonProps;

    LOG(ERROR) << "SHIMFIX in this=" << static_cast<const void*>(thisptr)
               << " sensorId=" << props.commonProps.sensorId
               << " strength=" << static_cast<int>(props.commonProps.sensorStrength)
               << " maxEnroll=" << props.commonProps.maxEnrollmentsPerUser
               << " componentInfo.size=" << props.commonProps.componentInfo.size();

    props = SensorPropsInit(props);

    LOG(ERROR) << "SHIMFIX out sensorType=" << static_cast<int>(props.sensorType)
               << " locs.size=" << props.sensorLocations.size()
               << " halHandlesDisplayTouches=" << props.halHandlesDisplayTouches;

    return writeToParcel(&props, parcel);
}

// ============================================================================
// 汐 2026-10-10：第三处 ABI 错配 —— BnSession::getInterfaceVersion
//
// 铁证（反汇编 /vendor/lib64/android.hardware.biometrics.fingerprint-V2-ndk.so）：
//   BnSession::getInterfaceVersion 只有两条指令：
//       mov  w8, #0x2      <- 写死返回 2
//       str  w8, [x1]
//   它报 2，是因为这个 lib 是按 AIDL V2 生成的。
//
// 但 OPlus 的 HAL 实现里**只有 V1 那套方法**（enroll / authenticate / detectInteraction），
// V2 新加的 *WithContext 系列它没覆盖，全部落到 ISessionDefault 的兜底实现，
// 而兜底实现返回 EX_UNSUPPORTED_OPERATION。
//
// 后果链（已实测）：
//   HAL 报 V2
//     -> AidlSession.hasContextMethods() = (mHalInterfaceVersion >= 2) = true
//     -> FingerprintEnrollClient 调 session.enrollWithContext(...)   [V2 方法]
//     -> HAL 兜底返回 EX_UNSUPPORTED_OPERATION
//     -> ISession$Stub$Proxy 抛 UnsupportedOperationException
//     -> FingerprintEnrollClient.doEnroll 只 catch RemoteException，接不住
//     -> system_server 崩、系统重启
//
// 修法：把它摁回 1。框架于是走 V1 的 enroll() —— 那是 HAL 真的实现了的。
::ndk::ScopedAStatus ShimGetInterfaceVersion(void* /*thisptr*/, int32_t* out)
        asm("_ZN4aidl7android8hardware10biometrics11fingerprint9BnSession19getInterfaceVersionEPi");

::ndk::ScopedAStatus ShimGetInterfaceVersion(void* /*thisptr*/, int32_t* out) {
    if (out == nullptr) {
        LOG(ERROR) << "SHIMFIX getInterfaceVersion: out == NULL";
        return ::ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    LOG(ERROR) << "SHIMFIX getInterfaceVersion -> 1（本 HAL 只实现 V1 方法集）";
    *out = 1;
    return ::ndk::ScopedAStatus::ok();
}

// 汐 2026-10-10：补第二处 ABI 错配 —— BnSession::getInterfaceHash
// ----------------------------------------------------------------------------
// 症状：录入指纹时指纹 HAL 段错误（tombstone_10）：
//     signal 11, fault addr 0x0 (write)
//     __memcpy <- std::string::__assign_no_alias <- BnSession::getInterfaceHash(std::string*)+120
//     崩溃库: /vendor/lib64/android.hardware.biometrics.fingerprint-V2-ndk.so
//
// 反汇编判定（2026-10-10，llvm-objdump on the device's prebuilt）：
//   函数本身用的是标准 NDK 约定（返回值走 x8 槽、out 走 x1），代码没问题；
//   是调用方 ISession_onTransact 把 0 放进了 x1。
//   ⇒ 同一个库里两半生成代码的虚表/约定对不上（OPlus 按旧 binder-ndk 编的 VNDK 预编译件
//     撞上 AOSP 16 框架）。
//
// 处置：像覆盖 writeToParcel 那样，把这个符号也接管掉 ——
//   out 有效 → 写入正确的 ISession v2 接口哈希；
//   out 为 NULL（正是崩溃那种情况）→ 不写，返回「不支持」，让框架自行降级走版本号。
//   关键是【绝不往 NULL 写】。
//
// 哈希来源：hardware/interfaces/biometrics/fingerprint/aidl/
//              aidl_api/android.hardware.biometrics.fingerprint/2/.hash
//   并已与设备上预编译件 .rodata 里的串（c2f3b863b6dff925bc4451473ee6caa1）对上。
// ============================================================================
namespace {
constexpr char kISessionV2InterfaceHash[] = "c2f3b863b6dff925bc4451473ee6caa1aa304b8f";
}  // anonymous namespace

// 用 asm() 标签给符号改名：既保留 C++ 语义（ScopedAStatus 是类，不能用 extern "C"），
// 又让导出的 mangled name 正好是 BnSession::getInterfaceHash，从而抢在预编译件前面。
::ndk::ScopedAStatus ShimGetInterfaceHash(void* /*thisptr*/, std::string* out)
        asm("_ZN4aidl7android8hardware10biometrics11fingerprint9BnSession16getInterfaceHash"
            "EPNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEE");

::ndk::ScopedAStatus ShimGetInterfaceHash(void* /*thisptr*/, std::string* /*out*/) {
    // 2026-10-10 修复：无条件返回异常，绝不碰 out。
    // 铁证（tombstone，崩在录入指纹时）：
    //   #00 std::string::__assign_external+204      (write to 0x0)
    //   #01 BnSession::getInterfaceHash(std::string*)+32   <- 本函数
    //   #02 _ISession_onTransact+2428                      <- OPlus prebuilt 里的旧生成代码
    // 崩因不是 out==nullptr，而是 out 指向的 std::string 从未被构造
    // （libc++ 合法空串的 __data_ 指向自身短串缓冲，绝不会是 NULL）。
    // OPlus 那份 onTransact 与我们的 BnSession ABI 对不上，把一块未构造内存当 string 传了进来。
    // => 本符号只需存在（满足链接），行为必须是「立刻抛异常、不写任何参数」。
    LOG(ERROR) << "SHIMFIX getInterfaceHash: 无条件不写 out（ABI 错配，写了必崩）";
    return ::ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
