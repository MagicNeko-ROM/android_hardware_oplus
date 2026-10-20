/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * vendor.lineage.oplus_als 服务端 —— 向 sensors HAL 提供「屏幕上方区域的平均色」。
 *
 * 为什么必须是独立进程而不是写进 sensors HAL：
 *   截图要经 SurfaceFlinger，而 SF 的权限检查是
 *     if ((uid != AID_GRAPHICS) && !checkPermission(READ_FRAME_BUFFER, pid, uid)) 拒绝;
 *   跑在 graphics(1021) 这个 uid 上就天然免权限。sensors HAL 是 vendor 域、链纯 vendor 库，
 *   把 SF 拉进去要同时踩 VNDK 与 SELinux 两颗雷。分成独立服务，两边都干净。
 */
#define LOG_TAG "oplus-als-service"

#include "AreaCapture.h"

#include <aidl/vendor/lineage/oplus_als/BnAreaCapture.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::vendor::lineage::oplus_als::AreaRgbCaptureResult;
using aidl::vendor::lineage::oplus_als::BnAreaCapture;
using aidl::vendor::lineage::oplus_als::areaCaptureInit;
using aidl::vendor::lineage::oplus_als::captureAreaBrightness;

namespace {

class AreaCaptureImpl final : public BnAreaCapture {
  public:
    ndk::ScopedAStatus getAreaBrightness(AreaRgbCaptureResult* out) override {
        *out = captureAreaBrightness();
        return ndk::ScopedAStatus::ok();
    }
};

}  // namespace

int main() {
    android::base::InitLogging(nullptr, android::base::KernelLogger);

    if (!areaCaptureInit()) {
        LOG(WARNING) << "初始化失败 —— 仍会注册服务，但返回值恒为 0";
    }

    std::shared_ptr<AreaCaptureImpl> service = ndk::SharedRefBase::make<AreaCaptureImpl>();
    const std::string instance =
            std::string() + AreaCaptureImpl::descriptor + "/default";
    binder_status_t status =
            AServiceManager_addService(service->asBinder().get(), instance.c_str());
    if (status != STATUS_OK) {
        LOG(ERROR) << "注册 " << instance << " 失败: " << status;
        return 1;
    }
    LOG(INFO) << "已注册 " << instance;

    ABinderProcess_setThreadPoolMaxThreadCount(2);
    ABinderProcess_joinThreadPool();
    return 0;
}
