/*
 * SPDX-FileCopyrightText: 2019-2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "DisplayModesService"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <fcntl.h>
#include <livedisplay/oplus/DisplayModes.h>
#include <oplus/oplus_display_panel.h>
#include <fstream>

namespace aidl {
namespace vendor {
namespace lineage {
namespace livedisplay {

static const std::string kModeBasePath = "/sys/class/drm/card0-DSI-1/";
static const std::string kDefaultPath = "/data/vendor/display/default_display_mode";

// Mode ids here must match qdcm display mode ids
// [2026-10-12 xi v6] 重映射 displayModeId（注意 ModeInfo 字段顺序！）
//
// 结构体定义（DisplayModes.h:33）：
//     struct ModeInfo {
//         std::string name;        // 1
//         int32_t displayModeId;   // 2  <- 要改的是这个
//         uint32_t seedMode;       // 3  <- 面板 SEED，不是这个
//     };
// v5 曾把值写到第 3 个字段（seedMode）上，日志表现为
// "[RM692E5] Invalid seed mode 7/9"，而 displayModeId 纹丝不动。已修正。
//
// 本机 BOE RM692E5 的 SDM 实际只报告 3 个 display mode
// （dumpsys SurfaceFlinger "color modes supported"）：
//     mode 0  RIs { 0 }                          -> NATIVE
//     mode 7  RIs { 0 256 257 258 259 ... 300 }  -> SRGB     (COLOR_MODE_BOOSTED=256 挂此)
//     mode 9  RIs { 0 256 257 }                  -> DISPLAY_P3
// 原表的 0/1/2/4 中，1 与 4 在本机不存在，SDM 会打
// "Failed to get color mode name for mode id = N" 后静默失败。
// seedMode 保持原值（面板 SEED 对 BOE 无定义，内核会打 Invalid seed mode，无害）。
const std::map<int32_t, DisplayModes::ModeInfo> DisplayModes::kModeMap = {
        {0, {"Vivid", 7, 0}},
        {1, {"Natural", 0, 1}},
        {2, {"Cinematic", 9, 0}},
        {3, {"Brilliant", 9, 4}},
};

DisplayModes::DisplayModes(std::shared_ptr<sdm::SDMController> controller)
    : mController(controller),
      mOplusDisplayFd(open("/dev/oplus_display", O_RDWR)),
      mCurrentModeId(0),
      mDefaultModeId(0) {
    std::ifstream defaultFile(kDefaultPath);

    defaultFile >> mDefaultModeId;
    LOG(DEBUG) << "Default file read result " << mDefaultModeId << " fail " << defaultFile.fail();

    // [2026-10-12 xi] 构造时不再自动应用默认模式。
    // 原实现在服务启动瞬间就执行 setDisplayMode()：既写面板 SEED ioctl
    // (PANEL_IOCTL_SET_SEED)、又切 SDM 的 active display mode。而这里硬编码的
    // kModeMap 未必与该面板实际支持的 qdcm mode 对得上 —— 实测后果是
    // LocalDisplayAdapter 报 "DisplayModeSpecs base mode no longer available"，
    // 显示模式被切到系统不认的状态、UI 卡死（可锁屏，adb 正常）。
    // 改为只登记当前模式，等框架/用户显式 setDisplayMode 时再应用。
    if (kModeMap.find(mDefaultModeId) != kModeMap.end()) {
        mCurrentModeId = mDefaultModeId;
    } else {
        mCurrentModeId = 0;
    }

    // [2026-10-12 xi] 记录 SDMController 可用性，供 setDisplayMode 分支判断。
    LOG(INFO) << "DisplayModes initialized: sdmController=" << (mController ? "yes" : "no")
              << " oplusDisplayFd=" << mOplusDisplayFd;
}

// Methods from ::aidl::vendor::lineage::livedisplay::BnDisplayModes follow.
ndk::ScopedAStatus DisplayModes::getDisplayModes(std::vector<DisplayMode>* _aidl_return) {
    std::vector<DisplayMode> modes;

    for (const auto& entry : kModeMap) {
        modes.push_back({entry.first, entry.second.name});
    }
    *_aidl_return = modes;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus DisplayModes::getCurrentDisplayMode(DisplayMode* _aidl_return) {
    *_aidl_return = {mCurrentModeId, kModeMap.at(mCurrentModeId).name};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus DisplayModes::getDefaultDisplayMode(DisplayMode* _aidl_return) {
    *_aidl_return = {mDefaultModeId, kModeMap.at(mDefaultModeId).name};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus DisplayModes::setDisplayMode(int32_t modeID, bool makeDefault) {
    const auto iter = kModeMap.find(modeID);
    if (iter == kModeMap.end()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    // [2026-10-12 xi] SDMController 可能为 nullptr（LiveDisplay HAL 在 SDM 不可用时
    // 仍要提供面板 seed 切换能力）。逐条判空，绝不解引用空指针 —— 这是 A 方案的核心：
    // 把「SDM 起不来就整个服务崩」降级为「SDM 起不来仅影响 QDCM 色彩处理」。
    if (mOplusDisplayFd >= 0) {
        ioctl(mOplusDisplayFd, PANEL_IOCTL_SET_SEED, &iter->second.seedMode);
    } else {
        LOG(WARNING) << "setDisplayMode: /dev/oplus_display unavailable, SEED not applied";
    }

    if (mController) {
        mController->setActiveDisplayMode(iter->second.displayModeId);
    } else {
        LOG(WARNING) << "setDisplayMode: no SDMController, QDCM mode " << iter->second.displayModeId
                     << " not applied";
    }

    mCurrentModeId = iter->first;
    if (makeDefault) {
        std::ofstream defaultFile(kDefaultPath);
        defaultFile << iter->first;
        if (!defaultFile.fail()) {
            if (mController) {
                mController->setDefaultDisplayMode(iter->second.displayModeId);
            }
            mDefaultModeId = iter->first;
        }
    }
    if (mOnDisplayModeSet) {
        mOnDisplayModeSet();
    }
    return ndk::ScopedAStatus::ok();
}

}  // namespace livedisplay
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
