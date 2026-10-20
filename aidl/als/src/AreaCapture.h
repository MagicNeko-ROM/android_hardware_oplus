/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * 取「环境光传感器正上方那块屏幕区域」的平均颜色。
 * 用途：sensors HAL 的 ALS 补偿需要知道屏幕自己贡献了多少光，
 *       它用这个平均值去校正原始 lux 读数（见 AlsCorrection.cpp）。
 *
 * 与 OPlus 原实现的差别：原厂走 IAreaCapture AIDL 独立服务；这里同样是独立服务，
 * 但区域与降采样策略是我们自己的（见 .cpp 顶部说明）。
 */
#pragma once

#include <aidl/vendor/lineage/oplus_als/AreaRgbCaptureResult.h>

namespace aidl::vendor::lineage::oplus_als {

// 初始化与 SurfaceFlinger 的连接。失败返回 false（调用方应视为「本服务不可用」）。
bool areaCaptureInit();

// 取一次平均颜色。返回值 r/g/b 归一化到 0.0~1.0（与原实现约定一致）。
::aidl::vendor::lineage::oplus_als::AreaRgbCaptureResult captureAreaBrightness();

}  // namespace aidl::vendor::lineage::oplus_als
