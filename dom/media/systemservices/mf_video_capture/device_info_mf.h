/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_SYSTEMSERVICES_MF_VIDEO_CAPTURE_DEVICE_INFO_MF_H_
#define DOM_MEDIA_SYSTEMSERVICES_MF_VIDEO_CAPTURE_DEVICE_INFO_MF_H_

#include <atomic>
#include <memory>
#include <vector>

#include "api/sequence_checker.h"
#include "mf_capture_utils.h"
#include "modules/video_capture/device_info_impl.h"
#include "mozilla/Maybe.h"

namespace webrtc::videocapturemodule {

/**
 * DeviceInfo implementation for the Media Foundation camera backend.
 *
 * Single threaded. DeviceChange() is called by the platform device-change
 * monitor, on the same video capture thread.
 *
 * Unlike the AVFoundation backend, capabilities are enumerated lazily per
 * device rather than for all devices up front: enumerating a device's
 * capabilities means activating its IMFMediaSource, which opens the camera.
 * This matches the DirectShow backend, which only builds a capability map for
 * the device it is asked about.
 */
class DeviceInfoMF : public DeviceInfoImpl {
 public:
  DeviceInfoMF();
  virtual ~DeviceInfoMF();

  // Implementation of DeviceInfoImpl.
  int32_t Init() override { return 0; }
  void DeviceChange() override;
  uint32_t NumberOfDevices() override;
  int32_t GetDeviceName(uint32_t aDeviceNumber, char* aDeviceNameUTF8,
                        uint32_t aDeviceNameLength, char* aDeviceUniqueIdUTF8,
                        uint32_t aDeviceUniqueIdUTF8Length,
                        char* aProductUniqueIdUTF8 = nullptr,
                        uint32_t aProductUniqueIdUTF8Length = 0,
                        pid_t* aPid = nullptr,
                        bool* aDeviceIsPlaceholder = nullptr) override;
  int32_t NumberOfCapabilities(const char* aDeviceUniqueIdUTF8) override;
  int32_t GetCapability(const char* aDeviceUniqueIdUTF8,
                        uint32_t aDeviceCapabilityNumber,
                        VideoCaptureCapability& aCapability) override;
  int32_t DisplayCaptureSettingsDialogBox(const char* aDeviceUniqueIdUTF8,
                                          const char* aDialogTitleUTF8,
                                          void* aParentWindow,
                                          uint32_t aPositionX,
                                          uint32_t aPositionY) override {
    return -1;
  }
  int32_t CreateCapabilityMap(const char* aDeviceUniqueIdUTF8) override
      RTC_EXCLUSIVE_LOCKS_REQUIRED(_apiLock);

 private:
  struct Device {
    MFCaptureDevice mDevice;
    // Nothing() until the capabilities of this device have been enumerated.
    mozilla::Maybe<nsTArray<VideoCaptureCapability>> mCapabilities;
    // Media Foundation always hands out an IMFActivate, so its absence marks a
    // device that came from DirectShow and that mDirectShowInfo owns.
    bool IsDirectShow() const { return !mDevice.mActivate; }
  };

  void EnsureDeviceList();
  // Appends the devices DirectShow reports that Media Foundation does not,
  // which is how virtual cameras registered as DirectShow filters stay visible.
  void AppendDirectShowOnlyDevices();
  // Drops the device list, releasing its COM references in the multithreaded
  // apartment.
  void ReleaseDevices();
  Device* FindDevice(const char* aDeviceUniqueIdUTF8);
  // Enumerates aDevice's capabilities if that has not been done yet. Returns
  // nullptr if the device could not be opened.
  const nsTArray<VideoCaptureCapability>* EnsureCapabilities(Device& aDevice);

  SequenceChecker mChecker;
  std::atomic<bool> mInvalidateDevices;
  nsTArray<Device> mDevices RTC_GUARDED_BY(mChecker);
  // Created on first use, and only to enumerate and drive the devices Media
  // Foundation does not report.
  std::unique_ptr<VideoCaptureModule::DeviceInfo> mDirectShowInfo
      RTC_GUARDED_BY(mChecker);
};

}  // namespace webrtc::videocapturemodule

#endif
