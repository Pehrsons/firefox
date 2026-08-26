/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <windows.devices.enumeration.h>
#include <windows.h>
#include <wrl.h>

#include <utility>
#include <vector>

#include "CameraRotation.h"
#include "WinUtils.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/mscom/EnsureMTA.h"
#include "nsIObserver.h"
#include "nsIObserverService.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "nsWindowsHelpers.h"
#include "rtc_base/logging.h"

using ABI::Windows::Devices::Enumeration::DeviceClass;
using ABI::Windows::Devices::Enumeration::DeviceClass_VideoCapture;
using ABI::Windows::Devices::Enumeration::DeviceInformationCollection;
using ABI::Windows::Devices::Enumeration::IDeviceInformation;
using ABI::Windows::Devices::Enumeration::IDeviceInformationStatics;
using ABI::Windows::Devices::Enumeration::IEnclosureLocation;
using ABI::Windows::Devices::Enumeration::Panel;
using ABI::Windows::Devices::Enumeration::Panel_Back;
using ABI::Windows::Devices::Enumeration::Panel_Front;
using ABI::Windows::Foundation::IAsyncOperation;
using ABI::Windows::Foundation::IAsyncOperationCompletedHandler;
using ABI::Windows::Foundation::Collections::IVectorView;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;

namespace mozilla::camera {

namespace {

// Which side of the device enclosure a camera is mounted on. Unknown covers
// external cameras, which do not turn with the device, as well as any camera
// that could not be classified.
enum class CameraPanel : uint8_t { Unknown, Front, Back };

// Everything before the trailing device interface class GUID. A Media
// Foundation symbolic link, a DirectShow DevicePath and a WinRT
// DeviceInformation::Id for one camera agree on this part and disagree on the
// GUID.
nsDependentCSubstring DeviceInstancePrefix(const nsACString& aId) {
  const int32_t index = aId.RFind("#{"_ns);
  return nsDependentCSubstring(aId, 0,
                               index < 0 ? aId.Length() : uint32_t(index));
}

bool IsSameDevice(const nsACString& aIdA, const nsACString& aIdB) {
  return DeviceInstancePrefix(aIdA).Equals(DeviceInstancePrefix(aIdB),
                                           nsCaseInsensitiveCStringComparator);
}

using FindAllOperation = IAsyncOperation<DeviceInformationCollection*>;
using DeviceInformationVector =
    IVectorView<ABI::Windows::Devices::Enumeration::DeviceInformation*>;

CameraPanel GetCameraPanelMTA(const nsACString& aDeviceUniqueId) {
  ComPtr<IDeviceInformationStatics> statics;
  HRESULT hr = ::RoGetActivationFactory(
      HStringReference(
          RuntimeClass_Windows_Devices_Enumeration_DeviceInformation)
          .Get(),
      IID_PPV_ARGS(&statics));
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "No DeviceInformation factory, 0x" << std::hex << hr;
    return CameraPanel::Unknown;
  }

  ComPtr<FindAllOperation> operation;
  hr = statics->FindAllAsyncDeviceClass(DeviceClass_VideoCapture, &operation);
  if (FAILED(hr)) {
    return CameraPanel::Unknown;
  }

  // WinRT offers no synchronous device enumeration, and this is called from a
  // context that already blocks.
  nsAutoHandle done(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!done) {
    return CameraPanel::Unknown;
  }
  HANDLE doneHandle = done.get();
  hr = operation->put_Completed(
      Callback<IAsyncOperationCompletedHandler<DeviceInformationCollection*>>(
          [doneHandle](FindAllOperation*,
                       ABI::Windows::Foundation::AsyncStatus) -> HRESULT {
            ::SetEvent(doneHandle);
            return S_OK;
          })
          .Get());
  if (FAILED(hr)) {
    return CameraPanel::Unknown;
  }
  // Bounded so that a wedged enumeration cannot hang capture startup.
  if (::WaitForSingleObject(doneHandle, 3000) != WAIT_OBJECT_0) {
    RTC_LOG(LS_WARNING) << "Timed out enumerating devices for camera rotation";
    return CameraPanel::Unknown;
  }

  ComPtr<DeviceInformationVector> devices;
  hr = operation->GetResults(&devices);
  if (FAILED(hr) || !devices) {
    RTC_LOG(LS_WARNING) << "DeviceInformation::FindAllAsync failed, 0x"
                        << std::hex << hr;
    return CameraPanel::Unknown;
  }

  unsigned int count = 0;
  if (FAILED(devices->get_Size(&count))) {
    return CameraPanel::Unknown;
  }

  for (unsigned int i = 0; i < count; ++i) {
    ComPtr<IDeviceInformation> device;
    if (FAILED(devices->GetAt(i, &device))) {
      continue;
    }
    HSTRING id = nullptr;
    if (FAILED(device->get_Id(&id))) {
      continue;
    }
    unsigned int length = 0;
    const wchar_t* wide = ::WindowsGetStringRawBuffer(id, &length);
    if (!wide) {
      continue;
    }
    const NS_ConvertUTF16toUTF8 deviceId(
        reinterpret_cast<const char16_t*>(wide), length);
    if (!IsSameDevice(aDeviceUniqueId, deviceId)) {
      continue;
    }

    ComPtr<IEnclosureLocation> location;
    if (FAILED(device->get_EnclosureLocation(&location)) || !location) {
      // No enclosure location means the camera is not part of the enclosure,
      // i.e. it is external.
      return CameraPanel::Unknown;
    }
    Panel panel;
    if (FAILED(location->get_Panel(&panel))) {
      return CameraPanel::Unknown;
    }
    switch (panel) {
      case Panel_Front:
        return CameraPanel::Front;
      case Panel_Back:
        return CameraPanel::Back;
      default:
        return CameraPanel::Unknown;
    }
  }

  return CameraPanel::Unknown;
}

bool IsAutoRotationEnabled() {
  AR_STATE state{};
  if (!widget::WinUtils::GetAutoRotationState(&state)) {
    return false;
  }
  // AR_ENABLED is 0, while the rest of AR_STATE is a bitfield of reasons that
  // auto-rotation is unavailable or suppressed.
  return state == AR_ENABLED;
}

// True if aDeviceName is an active built-in panel rather than an external
// monitor.
bool IsInternalDisplay(const wchar_t* aDeviceName) {
  UINT32 numPaths = 0;
  UINT32 numModes = 0;
  if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPaths,
                                    &numModes) != ERROR_SUCCESS) {
    return false;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);
  if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPaths, paths.data(),
                           &numModes, modes.data(), nullptr) != ERROR_SUCCESS) {
    return false;
  }
  paths.resize(numPaths);

  for (const auto& path : paths) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
      continue;
    }
    if (wcscmp(source.viewGdiDeviceName, aDeviceName) != 0) {
      continue;
    }
    return path.targetInfo.outputTechnology ==
           DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL;
  }
  return false;
}

// Fills aMode with the current settings of the active built-in display.
bool GetInternalDisplayMode(DEVMODEW& aMode) {
  DISPLAY_DEVICEW device{};
  device.cb = sizeof(device);
  for (DWORD index = 0; ::EnumDisplayDevicesW(nullptr, index, &device, 0);
       ++index) {
    if (!(device.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
      continue;
    }
    if (!IsInternalDisplay(device.DeviceName)) {
      continue;
    }
    aMode = {};
    aMode.dmSize = sizeof(aMode);
    return ::EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS,
                                  &aMode) != 0;
  }
  return false;
}

// Whether the display is taller than it is wide in its unrotated orientation,
// which is what distinguishes a tablet-style device from a laptop-style one.
bool IsPortraitDevice(const DEVMODEW& aMode) {
  uint32_t width = aMode.dmPelsWidth;
  uint32_t height = aMode.dmPelsHeight;
  if (aMode.dmDisplayOrientation == DMDO_90 ||
      aMode.dmDisplayOrientation == DMDO_270) {
    std::swap(width, height);
  }
  return height > width;
}

CameraPanel GetCameraPanel(const nsACString& aDeviceUniqueId) {
  CameraPanel panel = CameraPanel::Unknown;
  const nsCString id(aDeviceUniqueId);
  mozilla::mscom::EnsureMTA([&] { panel = GetCameraPanelMTA(id); });
  return panel;
}

webrtc::VideoRotation RotationForPanel(CameraPanel aPanel,
                                       const DEVMODEW& aMode) {
  const bool portrait = IsPortraitDevice(aMode);
  const bool rear = aPanel == CameraPanel::Back;
  // Degrees clockwise that the camera is mounted relative to the display in its
  // current orientation.
  int offset = 0;
  switch (aMode.dmDisplayOrientation) {
    case DMDO_DEFAULT:
      offset = portrait ? (rear ? 270 : 90) : 0;
      break;
    case DMDO_90:
      offset = portrait ? 180 : (rear ? 270 : 90);
      break;
    case DMDO_180:
      offset = portrait ? (rear ? 90 : 270) : 180;
      break;
    case DMDO_270:
      offset = portrait ? 0 : (rear ? 90 : 270);
      break;
    default:
      return webrtc::kVideoRotation_0;
  }

  switch ((360 - offset) % 360) {
    case 90:
      return webrtc::kVideoRotation_90;
    case 180:
      return webrtc::kVideoRotation_180;
    case 270:
      return webrtc::kVideoRotation_270;
    default:
      return webrtc::kVideoRotation_0;
  }
}

// Re-evaluates camera rotation when the screen orientation changes. Device
// orientation changes generate WM_DISPLAYCHANGE, which is what refreshes
// ScreenManager and produces this notification.
class ScreenRotationObserver final : public nsIObserver {
 public:
  NS_DECL_ISUPPORTS

  ScreenRotationObserver(nsISerialEventTarget* aVideoCaptureThread,
                         std::function<void()>&& aOnChange)
      : mVideoCaptureThread(aVideoCaptureThread),
        mOnChange(std::move(aOnChange)) {}

  NS_IMETHOD Observe(nsISupports*, const char*, const char16_t*) override {
    MOZ_ASSERT(NS_IsMainThread());
    MOZ_ALWAYS_SUCCEEDS(mVideoCaptureThread->Dispatch(
        NS_NewRunnableFunction("CameraRotation::OnScreenChange",
                               [onChange = mOnChange] { onChange(); })));
    return NS_OK;
  }

 private:
  ~ScreenRotationObserver() = default;

  const nsCOMPtr<nsISerialEventTarget> mVideoCaptureThread;
  const std::function<void()> mOnChange;
};

NS_IMPL_ISUPPORTS(ScreenRotationObserver, nsIObserver)

StaticRefPtr<ScreenRotationObserver> sObserver;

}  // namespace

webrtc::VideoRotation GetCameraRotation(const nsACString& aDeviceUniqueId) {
  // Checked before looking the panel up, since that enumerates devices and no
  // conventional desktop or laptop can rotate its cameras at all.
  if (!IsAutoRotationEnabled()) {
    return webrtc::kVideoRotation_0;
  }
  DEVMODEW mode{};
  if (!GetInternalDisplayMode(mode)) {
    return webrtc::kVideoRotation_0;
  }
  // Not cached, since reaching here means hardware that can rotate, where this
  // runs only when capture starts or the screen orientation changes.
  const CameraPanel panel = GetCameraPanel(aDeviceUniqueId);
  if (panel == CameraPanel::Unknown) {
    return webrtc::kVideoRotation_0;
  }
  return RotationForPanel(panel, mode);
}

void StartCameraRotationMonitor(nsISerialEventTarget* aVideoCaptureThread,
                                std::function<void()>&& aOnChange) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!sObserver);
  sObserver =
      new ScreenRotationObserver(aVideoCaptureThread, std::move(aOnChange));
  if (nsCOMPtr<nsIObserverService> obs = services::GetObserverService()) {
    obs->AddObserver(sObserver, "screen-information-changed", false);
  }
}

void StopCameraRotationMonitor() {
  MOZ_ASSERT(NS_IsMainThread());
  if (!sObserver) {
    return;
  }
  if (nsCOMPtr<nsIObserverService> obs = services::GetObserverService()) {
    obs->RemoveObserver(sObserver, "screen-information-changed");
  }
  sObserver = nullptr;
}

}  // namespace mozilla::camera
