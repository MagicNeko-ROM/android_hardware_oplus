/*
 * SPDX-FileCopyrightText: 2019-2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.livedisplay-service-oplus"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <binder/ProcessState.h>
#include <livedisplay/oplus/AdaptiveBacklight.h>
#include <livedisplay/oplus/AntiFlicker.h>
#include <livedisplay/oplus/DisplayModes.h>
#include <livedisplay/oplus/SunlightEnhancement.h>
#include <livedisplay/sdm/PictureAdjustment.h>

using ::aidl::vendor::lineage::livedisplay::AdaptiveBacklight;
using ::aidl::vendor::lineage::livedisplay::AntiFlicker;
using ::aidl::vendor::lineage::livedisplay::DisplayModes;
using ::aidl::vendor::lineage::livedisplay::SunlightEnhancement;
using ::aidl::vendor::lineage::livedisplay::sdm::PictureAdjustment;
using ::aidl::vendor::lineage::livedisplay::sdm::SDMController;

int main() {
    android::ProcessState::self()->setThreadPoolMaxThreadCount(1);
    android::ProcessState::self()->startThreadPool();

    LOG(INFO) << "LiveDisplay HAL service is starting.";

    // [2026-10-13 xi v5] 回退到 v2 语义：ENABLE_DM 时绝不构造 SDMController。
    //
    // 演进史（四次实验的教训，别删）：
    //   v1 原始：ENABLE_DM || ENABLE_PA 就构造 SDM。当时后果是 SDMController
    //            构造路径触发 libsnapdragoncolor-manager.so 的
    //            StcOrderParserImpl::Init() 解引用空 this → SIGSEGV（见
    //            hal-revival/color-mode-forensics/tombstone_42.txt），
    //            composer / livedisplay 双双起不来。
    //   v2 规避：ENABLE_DM 时**不**构造 SDM，让服务能起来（但色彩切换退化为
    //            空操作：setDisplayMode 里 mController 为 null，QDCM 那步被跳过，
    //            DisplayModes 走 /dev/oplus_display 的 SEED 路径）。
    //   v3 现场：设备侧热推原厂 libtinyxml2.so（776 结构，MD5 cef84aa3…）后，
    //            SDM color-manager 初始化成功 —— dumpsys display 的
    //            mSupportedColorModes 由 [0] 变为 [0, 7, 9]。
    //   v4 冒进：以「v3 已消除根因」为由恢复构造 → 20261012 包翻车。
    //            实际落包形态是：原厂库以别名 libtinyxml2.oneplus.so 进包，
    //            而 libsnapdragoncolor-manager.so 的 DT_NEEDED 仍是
    //            libtinyxml2.so（AOSP 880 版，954b0ee2），STC 路径未通 ——
    //            实测该包 composer supportedColorModes=[0]，disp_api_init
    //            必败 → SDMController 构造 LOG(FATAL)
    //            "Failed to initialize SDM backend" → livedisplay 5 秒崩溃
    //            循环，连带 AdaptiveBacklight / DisplayModes 全部不可用。
    //            v4 的安全前提从未进包，这是 20261012 包的唯一回归点。
    //
    // v5 = v2 语义 + 本注释：Android 16 的最后一个包以「能用」优先。
    // 色彩模式的 QDCM 路径等 tinyxml2 垫片真正打通后再恢复（A17 周期处理）。
    //
    // 注意：Android 编译带 -fno-exceptions，这里不能用 try/catch。
    std::shared_ptr<SDMController> controller = nullptr;

    if (ENABLE_PA && !ENABLE_DM) {
        controller = std::make_shared<SDMController>();
        LOG(INFO) << "SDMController created (DM=" << ENABLE_DM << " PA=" << ENABLE_PA << ")";
    }

    std::shared_ptr<AdaptiveBacklight> ab =
            ENABLE_AB ? ndk::SharedRefBase::make<AdaptiveBacklight>() : nullptr;
    std::shared_ptr<AntiFlicker> af = ENABLE_AF ? ndk::SharedRefBase::make<AntiFlicker>() : nullptr;
    std::shared_ptr<DisplayModes> dm =
            ENABLE_DM ? ndk::SharedRefBase::make<DisplayModes>(controller) : nullptr;
    std::shared_ptr<PictureAdjustment> pa =
            ENABLE_PA ? ndk::SharedRefBase::make<PictureAdjustment>(controller) : nullptr;
    std::shared_ptr<SunlightEnhancement> se =
            ENABLE_SE ? ndk::SharedRefBase::make<SunlightEnhancement>() : nullptr;

    if (ab) {
        std::string instance = std::string() + AdaptiveBacklight::descriptor + "/default";
        binder_status_t status = AServiceManager_addService(ab->asBinder().get(), instance.c_str());
        // [2026-10-12 xi] 由 CHECK_EQ 改为软失败：任何一个 HAL 注册失败（常见于
        // VINTF manifest 与二进制接口集合不一致，status=EX_ILLEGAL_ARGUMENT=-3）
        // 都不再 abort 整个进程。原实现里一个子系统出问题就会让 livedisplay
        // 无限重启，连带 DisplayModes 也永远起不来。
        if (status != STATUS_OK) {
            LOG(ERROR) << "Failed to add service " << instance << ", status=" << status
                       << " (continuing; this interface stays unavailable)";
        } else {
            LOG(INFO) << "Registered " << instance;
        }
    }

    if (af) {
        std::string instance = std::string() + AntiFlicker::descriptor + "/default";
        binder_status_t status = AServiceManager_addService(af->asBinder().get(), instance.c_str());
        // [2026-10-12 xi] 由 CHECK_EQ 改为软失败：任何一个 HAL 注册失败（常见于
        // VINTF manifest 与二进制接口集合不一致，status=EX_ILLEGAL_ARGUMENT=-3）
        // 都不再 abort 整个进程。原实现里一个子系统出问题就会让 livedisplay
        // 无限重启，连带 DisplayModes 也永远起不来。
        if (status != STATUS_OK) {
            LOG(ERROR) << "Failed to add service " << instance << ", status=" << status
                       << " (continuing; this interface stays unavailable)";
        } else {
            LOG(INFO) << "Registered " << instance;
        }
    }

    if (dm) {
        std::string instance = std::string() + DisplayModes::descriptor + "/default";
        binder_status_t status = AServiceManager_addService(dm->asBinder().get(), instance.c_str());
        // [2026-10-12 xi] 由 CHECK_EQ 改为软失败：任何一个 HAL 注册失败（常见于
        // VINTF manifest 与二进制接口集合不一致，status=EX_ILLEGAL_ARGUMENT=-3）
        // 都不再 abort 整个进程。原实现里一个子系统出问题就会让 livedisplay
        // 无限重启，连带 DisplayModes 也永远起不来。
        if (status != STATUS_OK) {
            LOG(ERROR) << "Failed to add service " << instance << ", status=" << status
                       << " (continuing; this interface stays unavailable)";
        } else {
            LOG(INFO) << "Registered " << instance;
        }
    }

    if (pa) {
        std::string instance = std::string() + PictureAdjustment::descriptor + "/default";
        binder_status_t status = AServiceManager_addService(pa->asBinder().get(), instance.c_str());
        // [2026-10-12 xi] 由 CHECK_EQ 改为软失败：任何一个 HAL 注册失败（常见于
        // VINTF manifest 与二进制接口集合不一致，status=EX_ILLEGAL_ARGUMENT=-3）
        // 都不再 abort 整个进程。原实现里一个子系统出问题就会让 livedisplay
        // 无限重启，连带 DisplayModes 也永远起不来。
        if (status != STATUS_OK) {
            LOG(ERROR) << "Failed to add service " << instance << ", status=" << status
                       << " (continuing; this interface stays unavailable)";
        } else {
            LOG(INFO) << "Registered " << instance;
        }
    }

    if (se) {
        std::string instance = std::string() + SunlightEnhancement::descriptor + "/default";
        binder_status_t status = AServiceManager_addService(se->asBinder().get(), instance.c_str());
        // [2026-10-12 xi] 由 CHECK_EQ 改为软失败：任何一个 HAL 注册失败（常见于
        // VINTF manifest 与二进制接口集合不一致，status=EX_ILLEGAL_ARGUMENT=-3）
        // 都不再 abort 整个进程。原实现里一个子系统出问题就会让 livedisplay
        // 无限重启，连带 DisplayModes 也永远起不来。
        if (status != STATUS_OK) {
            LOG(ERROR) << "Failed to add service " << instance << ", status=" << status
                       << " (continuing; this interface stays unavailable)";
        } else {
            LOG(INFO) << "Registered " << instance;
        }
    }

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // should not reach
}
