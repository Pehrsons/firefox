/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CameraDeviceChange.h"

// Platforms whose capture backends report device changes themselves, as the
// AVFoundation, V4L2, PipeWire and Android ones do.

namespace mozilla::camera {

void StartCameraDeviceChangeMonitor(std::function<void()>&& aOnChange) {}

void StopCameraDeviceChangeMonitor() {}

}  // namespace mozilla::camera
