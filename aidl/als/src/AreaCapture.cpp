/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * 实现说明（汐 2026-10-10）
 * ------------------------------------------------------------------
 * 取哪块区域：传感器 tcs3701 在屏幕顶部、听筒附近，取它正上方一小条，
 *   既贴合「屏幕照进传感器的那部分光」，又把截图成本压到最低。
 *
 * 为什么降采样到 16x8：HAL 每个事件都可能来问（约 250ms 一次），全屏截图太重。
 *   靠 DisplayCaptureArgs.width/height 让 SurfaceFlinger 直接缩到 16x8，
 *   真正要算的只有 128 个像素。
 *
 * 为什么做 gamma 还原：屏幕发出的光与像素值不是线性关系（sRGB 约 2.2 次幂），
 *   而传感器看到的是【光】。直接平均像素值会低估亮色块、高估暗色块。
 *   ⚠️ 2.2 是工程近似，真正的系数应在测试台上标定后再定。
 *
 * 调用方（sensors HAL）自己节流；本函数不做缓存。
 */
#include <vector>
#include "AreaCapture.h"

#include <android-base/logging.h>
#include <android/gui/DisplayCaptureArgs.h>
#include <gui/ScreenCaptureResults.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SyncScreenCaptureListener.h>
#include <ui/GraphicBufferMapper.h>
#include <cmath>
#include <cstdint>

namespace aidl::vendor::lineage::oplus_als {

using ::android::ScreenshotClient;
using ::android::SurfaceComposerClient;
using ::android::SyncScreenCaptureListener;
using ::android::gui::ARect;
using ::android::gui::DisplayCaptureArgs;
using ::android::gui::ScreenCaptureResults;

namespace {

// 传感器正上方那条区域（屏幕像素坐标）；面板 1080x2412
constexpr int32_t kSrcLeft   = 400;
constexpr int32_t kSrcTop    = 0;
constexpr int32_t kSrcRight  = 680;   // 宽 280
constexpr int32_t kSrcBottom = 120;   // 高 120

// 降采样目标：让 SF 直接缩到这里
constexpr int32_t kDstWidth  = 16;
constexpr int32_t kDstHeight = 8;

// sRGB 近似 gamma（待标定）
constexpr float kGamma = 2.2f;

bool gInited = false;
bool gFailed = false;

}  // namespace

bool areaCaptureInit() {
    if (gInited) return !gFailed;
    gInited = true;
    if (SurfaceComposerClient::getDefault() == nullptr) {
        LOG(ERROR) << "无法连接 SurfaceFlinger —— 区域捕获不可用";
        gFailed = true;
        return false;
    }
    LOG(INFO) << "已连接 SurfaceFlinger，区域捕获就绪";
    return true;
}

AreaRgbCaptureResult captureAreaBrightness() {
    AreaRgbCaptureResult out{0.0f, 0.0f, 0.0f};
    if (!areaCaptureInit()) return out;

    DisplayCaptureArgs args;
    // 2026-10-10: getInternalDisplayToken() 在新框架里已移除。
    // 改成「取物理屏 id 列表 -> 拿第一个的 token」，flatland 就是这么用的。
    const std::vector<::android::PhysicalDisplayId> displayIds =
            SurfaceComposerClient::getPhysicalDisplayIds();
    if (displayIds.empty()) {
        LOG(WARNING) << "拿不到物理屏 id（屏幕可能已关）";
        return out;
    }
    args.displayToken = SurfaceComposerClient::getPhysicalDisplayToken(displayIds[0]);
    if (args.displayToken == nullptr) {
        LOG(WARNING) << "拿不到内置屏 token（屏幕可能已关）";
        return out;
    }
    // 注意：DisplayCaptureArgs 是【嵌套】结构 —— 公共参数在 captureArgs 里
    ARect crop;
        crop.left = kSrcLeft;
        crop.top = kSrcTop;
        crop.right = kSrcRight;
        crop.bottom = kSrcBottom;
        args.captureArgs.sourceCrop = crop;
    args.captureArgs.pixelFormat =
            static_cast<int32_t>(::android::ui::PixelFormat::RGBA_8888);
    args.width = kDstWidth;
    args.height = kDstHeight;

    auto listener = ::android::sp<SyncScreenCaptureListener>::make();
    ::android::status_t err = ScreenshotClient::captureDisplay(args, listener);
    if (err != ::android::NO_ERROR) {
        LOG(WARNING) << "captureDisplay 失败: " << err;
        return out;
    }

    ScreenCaptureResults results = listener->waitForResults();
    if (results.buffer == nullptr) {
        LOG(WARNING) << "截图返回空 buffer";
        return out;
    }

    void* base = nullptr;
    const ::android::Rect bounds(kDstWidth, kDstHeight);
    if (::android::GraphicBufferMapper::get()
                .lock(results.buffer->getNativeBuffer()->handle,
                      ::android::GraphicBuffer::USAGE_SW_READ_OFTEN, bounds, &base)
                != ::android::NO_ERROR) {
        LOG(WARNING) << "GraphicBuffer 映射失败";
        return out;
    }

    double acc[3] = {0.0, 0.0, 0.0};
    const uint8_t* p = static_cast<const uint8_t*>(base);
    const size_t stride = static_cast<size_t>(results.buffer->getStride()) * 4;
    for (int32_t y = 0; y < kDstHeight; y++) {
        const uint8_t* row = p + static_cast<size_t>(y) * stride;
        for (int32_t x = 0; x < kDstWidth; x++) {
            for (int c = 0; c < 3; c++) {
                const float v = row[x * 4 + c] / 255.0f;
                acc[c] += std::pow(v, kGamma);
            }
        }
    }
    ::android::GraphicBufferMapper::get().unlock(results.buffer->getNativeBuffer()->handle);

    constexpr double kN = static_cast<double>(kDstWidth) * kDstHeight;
    out.r = static_cast<float>(acc[0] / kN);
    out.g = static_cast<float>(acc[1] / kN);
    out.b = static_cast<float>(acc[2] / kN);
    return out;
}

}  // namespace aidl::vendor::lineage::oplus_als
