/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CameraDeviceChange.h"

// windows.h must come first, and ks.h before ksmedia.h, so this block cannot be
// sorted.
// clang-format off
#include <windows.h>
#include <dbt.h>
#include <ks.h>
#include <ksmedia.h>
// clang-format on

#include <utility>

#include "mozilla/Assertions.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/UniquePtr.h"
#include "rtc_base/logging.h"

// Windows reports camera arrival and removal as WM_DEVICECHANGE to a window, so
// a hidden one is needed to receive them. Both Windows capture backends used to
// carry their own copy of this; the DirectShow one only had it because of a
// patch on top of upstream libwebrtc.

namespace mozilla::camera {

namespace {

constexpr wchar_t kWindowClassName[] = L"CameraDeviceChange";

class Monitor {
 public:
  static Monitor* Create(std::function<void()>&& aOnChange) {
    auto monitor = MakeUnique<Monitor>(std::move(aOnChange));
    return monitor->Init() ? monitor.release() : nullptr;
  }

  explicit Monitor(std::function<void()>&& aOnChange)
      : mOnChange(std::move(aOnChange)) {}

  ~Monitor() {
    if (mDeviceNotify) {
      UnregisterDeviceNotification(mDeviceNotify);
    }
    if (mWindow) {
      DestroyWindow(mWindow);
    }
    if (mWindowClassRegistered) {
      UnregisterClassW(kWindowClassName, Instance());
    }
  }

  void OnDeviceChange() const { mOnChange(); }

 private:
  static HINSTANCE Instance() {
    return reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr));
  }

  bool Init() {
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = &WndProc;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hInstance = Instance();
    mWindowClassRegistered = RegisterClassW(&windowClass) != 0;
    if (!mWindowClassRegistered) {
      RTC_LOG(LS_WARNING) << "Failed to register the device notification "
                             "window class, error "
                          << GetLastError();
      return false;
    }

    mWindow = CreateWindowW(kWindowClassName, nullptr, 0, CW_USEDEFAULT,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            nullptr, nullptr, Instance(), this);
    if (!mWindow) {
      RTC_LOG(LS_WARNING) << "Failed to create the device notification window, "
                             "error "
                          << GetLastError();
      return false;
    }

    DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = KSCATEGORY_VIDEO_CAMERA;
    mDeviceNotify = RegisterDeviceNotificationW(mWindow, &filter,
                                                DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!mDeviceNotify) {
      RTC_LOG(LS_WARNING) << "Failed to register for device notifications, "
                             "error "
                          << GetLastError();
    }
    return true;
  }

  static bool IsVideoDevice(DEV_BROADCAST_HDR* aHeader) {
    if (!aHeader || aHeader->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE) {
      return false;
    }
    auto* deviceInterface =
        reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(aHeader);
    return deviceInterface->dbcc_classguid == KSCATEGORY_VIDEO_CAMERA;
  }

  static LRESULT CALLBACK WndProc(HWND aWnd, UINT aMsg, WPARAM aWParam,
                                  LPARAM aLParam) {
    if (aMsg == WM_CREATE) {
      SetWindowLongPtr(
          aWnd, GWLP_USERDATA,
          reinterpret_cast<LONG_PTR>(
              reinterpret_cast<LPCREATESTRUCT>(aLParam)->lpCreateParams));
    } else if (aMsg == WM_DESTROY) {
      SetWindowLongPtr(aWnd, GWLP_USERDATA, 0);
    } else if (aMsg == WM_DEVICECHANGE) {
      auto* self =
          reinterpret_cast<Monitor*>(GetWindowLongPtr(aWnd, GWLP_USERDATA));
      if (self &&
          IsVideoDevice(reinterpret_cast<DEV_BROADCAST_HDR*>(aLParam))) {
        self->OnDeviceChange();
      }
    }
    return DefWindowProcW(aWnd, aMsg, aWParam, aLParam);
  }

  const std::function<void()> mOnChange;
  HWND mWindow = nullptr;
  HDEVNOTIFY mDeviceNotify = nullptr;
  bool mWindowClassRegistered = false;
};

StaticAutoPtr<Monitor> sMonitor;

}  // namespace

void StartCameraDeviceChangeMonitor(std::function<void()>&& aOnChange) {
  MOZ_ASSERT(!sMonitor);
  // The window must be created on a thread that pumps messages, which the
  // video capture thread is on Windows since desktop capture needs it.
  sMonitor = Monitor::Create(std::move(aOnChange));
}

void StopCameraDeviceChangeMonitor() { sMonitor = nullptr; }

}  // namespace mozilla::camera
