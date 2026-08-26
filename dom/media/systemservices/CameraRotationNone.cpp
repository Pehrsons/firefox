/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CameraRotation.h"

// Platforms where the capture backend either rectifies camera orientation
// itself, as on Android, or where the concept does not apply.

namespace mozilla::camera {

webrtc::VideoRotation GetCameraRotation(const nsACString& aDeviceUniqueId) {
  return webrtc::kVideoRotation_0;
}

void StartCameraRotationMonitor(nsISerialEventTarget* aVideoCaptureThread,
                                std::function<void()>&& aOnChange) {}

void StopCameraRotationMonitor() {}

}  // namespace mozilla::camera
