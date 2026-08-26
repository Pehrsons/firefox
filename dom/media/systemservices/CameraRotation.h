/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_SYSTEMSERVICES_CAMERAROTATION_H_
#define DOM_MEDIA_SYSTEMSERVICES_CAMERAROTATION_H_

#include <functional>

#include "api/video/video_rotation.h"
#include "nsISerialEventTarget.h"
#include "nsStringFwd.h"

namespace mozilla::camera {

/**
 * The rotation to apply to frames from aDeviceUniqueId so that they appear
 * upright at the device's current orientation, for instance on a convertible
 * laptop folded into tent mode.
 *
 * Returns kVideoRotation_0 for cameras that are not mounted in the enclosure,
 * since those do not turn with the device, and on platforms that do not report
 * camera orientation. Video capture thread only.
 */
webrtc::VideoRotation GetCameraRotation(const nsACString& aDeviceUniqueId);

/**
 * Arranges for aOnChange to run on aVideoCaptureThread whenever the result of
 * GetCameraRotation() may have changed, so that a device rotated mid-capture
 * keeps producing upright frames.
 *
 * Main thread only, and must be paired with StopCameraRotationMonitor().
 */
void StartCameraRotationMonitor(nsISerialEventTarget* aVideoCaptureThread,
                                std::function<void()>&& aOnChange);
void StopCameraRotationMonitor();

}  // namespace mozilla::camera

#endif
