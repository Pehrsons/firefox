/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_SYSTEMSERVICES_CAMERADEVICECHANGE_H_
#define DOM_MEDIA_SYSTEMSERVICES_CAMERADEVICECHANGE_H_

#include <functional>

namespace mozilla::camera {

/**
 * Arranges for aOnChange to run whenever cameras are added to or removed from
 * the system.
 *
 * Video capture thread only, and must be paired with
 * StopCameraDeviceChangeMonitor(). aOnChange runs on that same thread.
 *
 * Only platforms whose capture backends do not report device changes
 * themselves have an implementation; elsewhere this does nothing and the
 * backend keeps reporting them.
 */
void StartCameraDeviceChangeMonitor(std::function<void()>&& aOnChange);
void StopCameraDeviceChangeMonitor();

}  // namespace mozilla::camera

#endif
