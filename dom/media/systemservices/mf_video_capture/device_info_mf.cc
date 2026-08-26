/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "device_info_mf.h"

#include <dbt.h>
#include <ksmedia.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/logging.h"

namespace webrtc::videocapturemodule {

namespace {

constexpr wchar_t kWindowClassName[] = L"DeviceInfoMF";

bool IsVideoDevice(DEV_BROADCAST_HDR* aHeader) {
  if (!aHeader || aHeader->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE) {
    return false;
  }
  auto* deviceInterface =
      reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(aHeader);
  return deviceInterface->dbcc_classguid == KSCATEGORY_VIDEO_CAMERA;
}

LRESULT CALLBACK WndProc(HWND aWnd, UINT aMsg, WPARAM aWParam, LPARAM aLParam) {
  if (aMsg == WM_CREATE) {
    SetWindowLongPtr(
        aWnd, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(
            reinterpret_cast<LPCREATESTRUCT>(aLParam)->lpCreateParams));
  } else if (aMsg == WM_DESTROY) {
    SetWindowLongPtr(aWnd, GWLP_USERDATA, 0);
  } else if (aMsg == WM_DEVICECHANGE) {
    auto* self =
        reinterpret_cast<DeviceInfoMF*>(GetWindowLongPtr(aWnd, GWLP_USERDATA));
    if (self && IsVideoDevice(reinterpret_cast<DEV_BROADCAST_HDR*>(aLParam))) {
      self->DeviceChange();
    }
  }
  return DefWindowProcW(aWnd, aMsg, aWParam, aLParam);
}

HINSTANCE CurrentInstance() {
  return reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr));
}

}  // namespace

DeviceInfoMF::DeviceInfoMF()
    : mInvalidateDevices(false),
      mWindow(nullptr),
      mDeviceNotify(nullptr),
      mWindowClassRegistered(false) {
  RTC_DCHECK_RUN_ON(&mChecker);

  WNDCLASSW windowClass = {};
  windowClass.lpfnWndProc = &WndProc;
  windowClass.lpszClassName = kWindowClassName;
  windowClass.hInstance = CurrentInstance();
  mWindowClassRegistered = RegisterClassW(&windowClass) != 0;
  if (!mWindowClassRegistered) {
    RTC_LOG(LS_WARNING) << "Failed to register the device notification window "
                           "class, error "
                        << GetLastError();
    return;
  }

  mWindow = CreateWindowW(kWindowClassName, nullptr, 0, CW_USEDEFAULT,
                          CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr,
                          nullptr, CurrentInstance(), this);
  if (!mWindow) {
    RTC_LOG(LS_WARNING) << "Failed to create the device notification window, "
                           "error "
                        << GetLastError();
    return;
  }

  DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
  filter.dbcc_size = sizeof(filter);
  filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
  filter.dbcc_classguid = KSCATEGORY_VIDEO_CAMERA;
  mDeviceNotify = RegisterDeviceNotificationW(mWindow, &filter,
                                              DEVICE_NOTIFY_WINDOW_HANDLE);
  if (!mDeviceNotify) {
    RTC_LOG(LS_WARNING) << "Failed to register for device notifications, error "
                        << GetLastError();
  }
}

DeviceInfoMF::~DeviceInfoMF() {
  RTC_DCHECK_RUN_ON(&mChecker);
  ReleaseDevices();
  if (mDeviceNotify) {
    UnregisterDeviceNotification(mDeviceNotify);
  }
  if (mWindow) {
    DestroyWindow(mWindow);
  }
  if (mWindowClassRegistered) {
    UnregisterClassW(kWindowClassName, CurrentInstance());
  }
}

void DeviceInfoMF::DeviceChange() {
  mInvalidateDevices = true;
  DeviceInfo::DeviceChange();
}

uint32_t DeviceInfoMF::NumberOfDevices() {
  RTC_DCHECK_RUN_ON(&mChecker);
  EnsureDeviceList();
  return mDevices.Length();
}

int32_t DeviceInfoMF::GetDeviceName(
    uint32_t aDeviceNumber, char* aDeviceNameUTF8, uint32_t aDeviceNameLength,
    char* aDeviceUniqueIdUTF8, uint32_t aDeviceUniqueIdUTF8Length,
    char* aProductUniqueIdUTF8, uint32_t aProductUniqueIdUTF8Length,
    pid_t* /* aPid */, bool* /* aDeviceIsPlaceholder */) {
  RTC_DCHECK_RUN_ON(&mChecker);
  // Don't EnsureDeviceList() here, since this function depends on the device
  // index staying stable relative to the NumberOfDevices() that produced it.

  if (aDeviceNumber >= mDevices.Length()) {
    return -1;
  }

  const MFCaptureDevice& device = mDevices[aDeviceNumber].mDevice;

  if (aDeviceNameUTF8 && aDeviceNameLength > 0) {
    strncpy(aDeviceNameUTF8, device.mName.get(), aDeviceNameLength);
    aDeviceNameUTF8[aDeviceNameLength - 1] = '\0';
  }

  if (aDeviceUniqueIdUTF8 && aDeviceUniqueIdUTF8Length > 0) {
    strncpy(aDeviceUniqueIdUTF8, device.mUniqueId.get(),
            aDeviceUniqueIdUTF8Length);
    aDeviceUniqueIdUTF8[aDeviceUniqueIdUTF8Length - 1] = '\0';
  }

  if (aProductUniqueIdUTF8 && aProductUniqueIdUTF8Length > 0) {
    GetProductId(device.mUniqueId.get(), aProductUniqueIdUTF8,
                 aProductUniqueIdUTF8Length);
  }

  return 0;
}

int32_t DeviceInfoMF::NumberOfCapabilities(const char* aDeviceUniqueIdUTF8) {
  RTC_DCHECK_RUN_ON(&mChecker);

  Device* device = FindDevice(aDeviceUniqueIdUTF8);
  if (!device) {
    return 0;
  }
  const nsTArray<VideoCaptureCapability>* capabilities =
      EnsureCapabilities(*device);
  if (!capabilities) {
    return 0;
  }
  return static_cast<int32_t>(capabilities->Length());
}

int32_t DeviceInfoMF::GetCapability(const char* aDeviceUniqueIdUTF8,
                                    uint32_t aDeviceCapabilityNumber,
                                    VideoCaptureCapability& aCapability) {
  RTC_DCHECK_RUN_ON(&mChecker);

  Device* device = FindDevice(aDeviceUniqueIdUTF8);
  if (!device) {
    return -1;
  }
  const nsTArray<VideoCaptureCapability>* capabilities =
      EnsureCapabilities(*device);
  if (!capabilities || aDeviceCapabilityNumber >= capabilities->Length()) {
    return -1;
  }

  aCapability = (*capabilities)[aDeviceCapabilityNumber];
  return 0;
}

int32_t DeviceInfoMF::CreateCapabilityMap(const char* aDeviceUniqueIdUTF8) {
  RTC_DCHECK_RUN_ON(&mChecker);

  _captureCapabilities.clear();

  Device* device = FindDevice(aDeviceUniqueIdUTF8);
  if (!device) {
    RTC_LOG(LS_INFO) << "CreateCapabilityMap found no matching device";
    return -1;
  }
  const nsTArray<VideoCaptureCapability>* capabilities =
      EnsureCapabilities(*device);
  if (!capabilities) {
    return -1;
  }

  const size_t length = strlen(aDeviceUniqueIdUTF8);
  _lastUsedDeviceNameLength = static_cast<uint32_t>(length);
  _lastUsedDeviceName = static_cast<char*>(
      realloc(_lastUsedDeviceName, _lastUsedDeviceNameLength + 1));
  memcpy(_lastUsedDeviceName, aDeviceUniqueIdUTF8,
         _lastUsedDeviceNameLength + 1);

  _captureCapabilities.assign(capabilities->begin(), capabilities->end());
  return static_cast<int32_t>(_captureCapabilities.size());
}

void DeviceInfoMF::EnsureDeviceList() {
  RTC_DCHECK_RUN_ON(&mChecker);

  if (mInvalidateDevices.exchange(false)) {
    ReleaseDevices();
  }
  if (!mDevices.IsEmpty()) {
    return;
  }

  nsTArray<MFCaptureDevice> devices;
  if (FAILED(EnumerateCaptureDevices(devices))) {
    return;
  }
  mDevices.SetCapacity(devices.Length());
  for (auto& device : devices) {
    Device entry;
    entry.mDevice = std::move(device);
    mDevices.AppendElement(std::move(entry));
  }

  AppendDirectShowOnlyDevices();
}

void DeviceInfoMF::AppendDirectShowOnlyDevices() {
  RTC_DCHECK_RUN_ON(&mChecker);

  if (!mDirectShowInfo) {
    mDirectShowInfo.reset(webrtc::VideoCaptureFactory::CreateDeviceInfo());
    if (!mDirectShowInfo) {
      RTC_LOG(LS_WARNING) << "Failed to create the DirectShow device info, "
                             "DirectShow-only devices will be missing";
      return;
    }
  }

  const uint32_t count = mDirectShowInfo->NumberOfDevices();
  for (uint32_t i = 0; i < count; ++i) {
    char name[kVideoCaptureDeviceNameLength] = {};
    char uniqueId[kVideoCaptureUniqueNameLength] = {};
    if (mDirectShowInfo->GetDeviceName(i, name, sizeof(name), uniqueId,
                                       sizeof(uniqueId)) != 0) {
      continue;
    }
    if (std::any_of(mDevices.begin(), mDevices.end(),
                    [&](const Device& aOther) {
                      return IsSameDeviceInstance(aOther.mDevice.mUniqueId,
                                                  nsDependentCString(uniqueId));
                    })) {
      continue;
    }
    RTC_LOG(LS_INFO) << "Adding DirectShow-only capture device " << uniqueId;
    Device entry;
    entry.mDevice.mUniqueId = uniqueId;
    entry.mDevice.mName = name;
    mDevices.AppendElement(std::move(entry));
  }
}

void DeviceInfoMF::ReleaseDevices() {
  RTC_DCHECK_RUN_ON(&mChecker);

  if (mDevices.IsEmpty()) {
    return;
  }
  // RunInMTA() is synchronous, so mDevices would still only be touched under
  // mChecker, but the analyser treats the lambda body as a context of its own
  // and does not accept that. Annotating the lambda does not help: EnsureMTA
  // also compiles a path that stores it in a runnable, so the requirement
  // surfaces in nsThreadUtils.h instead. Moving the devices out sidesteps it; a
  // reference to the member would still be tracked. nsTArray leaves the
  // moved-from array empty, which std::vector does not promise.
  nsTArray<Device> devices = std::move(mDevices);
  RunInMTA([&] { devices.Clear(); });
}

DeviceInfoMF::Device* DeviceInfoMF::FindDevice(
    const char* aDeviceUniqueIdUTF8) {
  RTC_DCHECK_RUN_ON(&mChecker);

  if (!aDeviceUniqueIdUTF8) {
    return nullptr;
  }
  EnsureDeviceList();
  for (Device& device : mDevices) {
    // Media Foundation symbolic links, like DirectShow device paths, are not
    // guaranteed to keep their case between enumerations.
    if (device.mDevice.mUniqueId.Equals(nsDependentCString(aDeviceUniqueIdUTF8),
                                        nsCaseInsensitiveCStringComparator)) {
      return &device;
    }
  }
  return nullptr;
}

auto DeviceInfoMF::EnsureCapabilities(Device& aDevice)
    -> const nsTArray<VideoCaptureCapability>* {
  RTC_DCHECK_RUN_ON(&mChecker);

  if (aDevice.mCapabilities) {
    return aDevice.mCapabilities.ptr();
  }

  RTC_LOG(LS_INFO) << "Enumerating capabilities for device "
                   << aDevice.mDevice.mUniqueId.get();
  nsTArray<VideoCaptureCapability> capabilities;
  if (aDevice.IsDirectShow()) {
    if (!mDirectShowInfo) {
      return nullptr;
    }
    const char* uniqueId = aDevice.mDevice.mUniqueId.get();
    const int32_t count = mDirectShowInfo->NumberOfCapabilities(uniqueId);
    for (int32_t i = 0; i < count; ++i) {
      VideoCaptureCapability capability;
      if (mDirectShowInfo->GetCapability(uniqueId, static_cast<uint32_t>(i),
                                         capability) != 0) {
        return nullptr;
      }
      capabilities.AppendElement(capability);
    }
  } else if (FAILED(GetDeviceCapabilities(aDevice.mDevice.mActivate.Get(),
                                          capabilities))) {
    // Leave the capabilities unset so that a later attempt can succeed, for
    // instance once the device is no longer in use by another application.
    return nullptr;
  }
  aDevice.mCapabilities = mozilla::Some(std::move(capabilities));
  return aDevice.mCapabilities.ptr();
}

}  // namespace webrtc::videocapturemodule
