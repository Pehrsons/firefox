/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mf_capture_utils.h"

#include <mfreadwrite.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

#include "modules/video_capture/video_capture_defines.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/StaticPrefs_media.h"
#include "nsWindowsHelpers.h"
#include "rtc_base/logging.h"
#include "rtc_base/string_utils.h"

using Microsoft::WRL::ComPtr;

namespace webrtc::videocapturemodule {

namespace {

// MFEnumDeviceSources lives in mf.dll, not mfplat.dll, despite being declared
// alongside the mfplat entry points in mfidl.h.
HMODULE MfModule() {
  static HMODULE sModule = LoadLibrarySystem32(L"mf.dll");
  return sModule;
}

HMODULE MfReadWriteModule() {
  static HMODULE sModule = LoadLibrarySystem32(L"mfreadwrite.dll");
  return sModule;
}

nsCString ToUtf8(const wchar_t* aStr) {
  if (!aStr) {
    return nsCString();
  }
  return nsCString(NS_ConvertUTF16toUTF8(
      nsDependentString(reinterpret_cast<const char16_t*>(aStr))));
}

nsCString GetAllocatedStringAsUtf8(IMFActivate* aActivate,
                                   const GUID& aAttribute) {
  wchar_t* value = nullptr;
  UINT32 length = 0;
  if (FAILED(aActivate->GetAllocatedString(aAttribute, &value, &length))) {
    return nsCString();
  }
  auto free = mozilla::MakeScopeExit([&] { CoTaskMemFree(value); });
  return ToUtf8(value);
}

// Everything before the trailing device interface class GUID, which is the only
// part of the id that a Media Foundation symbolic link and a DirectShow
// DevicePath for the same device disagree on.
nsDependentCSubstring DeviceInstancePrefix(const nsACString& aId) {
  const int32_t index = aId.RFind("#{"_ns);
  return nsDependentCSubstring(aId, 0,
                               index < 0 ? aId.Length() : uint32_t(index));
}

HRESULT EnumerateCaptureDevicesMTA(nsTArray<MFCaptureDevice>& aDevices) {
  ComPtr<IMFAttributes> attributes;
  HRESULT hr = mozilla::wmf::MFCreateAttributes(&attributes, 1);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFCreateAttributes failed, 0x" << ToHex(hr);
    return hr;
  }
  hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr)) {
    return hr;
  }

  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  hr = MFEnumDeviceSourcesShim(attributes.Get(), &activates, &count);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFEnumDeviceSources failed, 0x" << ToHex(hr);
    return hr;
  }
  auto freeArray = mozilla::MakeScopeExit([&] { CoTaskMemFree(activates); });

  for (UINT32 i = 0; i < count; ++i) {
    MFCaptureDevice device;
    // MFEnumDeviceSources hands out references we now own.
    device.mActivate.Attach(activates[i]);
    activates[i] = nullptr;
    if (!device.mActivate) {
      continue;
    }
    device.mUniqueId = GetAllocatedStringAsUtf8(
        device.mActivate.Get(),
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
    device.mName = GetAllocatedStringAsUtf8(
        device.mActivate.Get(), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
    if (device.mUniqueId.IsEmpty()) {
      RTC_LOG(LS_WARNING) << "Skipping capture device with no symbolic link";
      continue;
    }
    if (device.mUniqueId.Length() >= kVideoCaptureUniqueNameLength) {
      RTC_LOG(LS_WARNING)
          << "Skipping capture device with too long symbolic link";
      continue;
    }
    if (device.mName.IsEmpty()) {
      device.mName = device.mUniqueId;
    }
    aDevices.AppendElement(std::move(device));
  }

  return S_OK;
}

HRESULT GetDeviceCapabilitiesMTA(
    IMFActivate* aActivate, nsTArray<VideoCaptureCapability>& aCapabilities) {
  ComPtr<IMFMediaSource> source;
  HRESULT hr = aActivate->ActivateObject(IID_PPV_ARGS(&source));
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "Failed to activate capture device, 0x" << ToHex(hr);
    return hr;
  }
  auto shutdown = mozilla::MakeScopeExit([&] {
    source = nullptr;
    (void)aActivate->ShutdownObject();
  });

  ComPtr<IMFPresentationDescriptor> descriptor;
  hr = source->CreatePresentationDescriptor(&descriptor);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "CreatePresentationDescriptor failed, 0x"
                        << ToHex(hr);
    return hr;
  }

  DWORD streamCount = 0;
  hr = descriptor->GetStreamDescriptorCount(&streamCount);
  if (FAILED(hr)) {
    return hr;
  }

  for (DWORD stream = 0; stream < streamCount; ++stream) {
    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> streamDescriptor;
    if (FAILED(descriptor->GetStreamDescriptorByIndex(stream, &selected,
                                                      &streamDescriptor))) {
      continue;
    }
    ComPtr<IMFMediaTypeHandler> handler;
    if (FAILED(streamDescriptor->GetMediaTypeHandler(&handler))) {
      continue;
    }
    GUID majorType = GUID_NULL;
    if (FAILED(handler->GetMajorType(&majorType)) ||
        majorType != MFMediaType_Video) {
      continue;
    }

    DWORD typeCount = 0;
    if (FAILED(handler->GetMediaTypeCount(&typeCount))) {
      continue;
    }
    for (DWORD type = 0; type < typeCount; ++type) {
      ComPtr<IMFMediaType> mediaType;
      if (FAILED(handler->GetMediaTypeByIndex(type, &mediaType))) {
        continue;
      }
      VideoCaptureCapability capability;
      if (!CapabilityFromMediaType(mediaType.Get(), capability)) {
        continue;
      }
      aCapabilities.AppendElement(capability);
      RTC_LOG(LS_INFO) << "Camera capability, width:" << capability.width
                       << " height:" << capability.height
                       << " type:" << static_cast<int>(capability.videoType)
                       << " fps:" << capability.maxFPS;
    }

    // Only the first video stream is used, mirroring the DirectShow backend's
    // use of a single PIN_CATEGORY_CAPTURE output pin.
    break;
  }

  return S_OK;
}

}  // namespace

HRESULT MFEnumDeviceSourcesShim(IMFAttributes* aAttributes,
                                IMFActivate*** aSources, UINT32* aCount) {
  using Fn = decltype(::MFEnumDeviceSources)*;
  static Fn sFn = nullptr;
  if (!sFn) {
    HMODULE module = MfModule();
    if (!module) {
      return E_FAIL;
    }
    sFn = reinterpret_cast<Fn>(GetProcAddress(module, "MFEnumDeviceSources"));
    if (!sFn) {
      return E_FAIL;
    }
  }
  return sFn(aAttributes, aSources, aCount);
}

HRESULT MFCreateSourceReaderFromMediaSourceShim(IMFMediaSource* aSource,
                                                IMFAttributes* aAttributes,
                                                IMFSourceReader** aReader) {
  using Fn = decltype(::MFCreateSourceReaderFromMediaSource)*;
  static Fn sFn = nullptr;
  if (!sFn) {
    HMODULE module = MfReadWriteModule();
    if (!module) {
      return E_FAIL;
    }
    sFn = reinterpret_cast<Fn>(
        GetProcAddress(module, "MFCreateSourceReaderFromMediaSource"));
    if (!sFn) {
      return E_FAIL;
    }
  }
  return sFn(aSource, aAttributes, aReader);
}

bool MediaFoundationCaptureEnabled() {
  if (!mozilla::StaticPrefs::
          media_webrtc_capture_camera_allow_mediafoundation_AtStartup()) {
    return false;
  }
  if (!mozilla::wmf::MediaFoundationInitializer::HasInitialized()) {
    RTC_LOG(LS_WARNING) << "Media Foundation is unavailable, falling back to "
                           "the DirectShow camera backend";
    return false;
  }
  return true;
}

VideoType VideoTypeFromMFSubtype(const GUID& aSubtype) {
  if (aSubtype == MFVideoFormat_I420) {
    return VideoType::kI420;
  }
  if (aSubtype == MFVideoFormat_IYUV) {
    return VideoType::kIYUV;
  }
  if (aSubtype == MFVideoFormat_YV12) {
    return VideoType::kYV12;
  }
  if (aSubtype == MFVideoFormat_NV12) {
    return VideoType::kNV12;
  }
  if (aSubtype == MFVideoFormat_YUY2) {
    return VideoType::kYUY2;
  }
  if (aSubtype == MFVideoFormat_UYVY) {
    return VideoType::kUYVY;
  }
  if (aSubtype == MFVideoFormat_MJPG) {
    return VideoType::kMJPEG;
  }
  if (aSubtype == MFVideoFormat_RGB24) {
    return VideoType::kRGB24;
  }
  return VideoType::kUnknown;
}

void GetProductId(const char* aSymbolicLink, char* aProductUniqueIdUTF8,
                  uint32_t aProductUniqueIdUTF8Length) {
  if (!aProductUniqueIdUTF8 || aProductUniqueIdUTF8Length == 0) {
    return;
  }
  *aProductUniqueIdUTF8 = '\0';
  if (!aSymbolicLink) {
    return;
  }

  const char* start = strstr(aSymbolicLink, "\\\\?\\");
  if (!start) {
    return;
  }
  start += 4;

  const char* pos = strchr(start, '&');
  if (!pos) {
    return;
  }
  // The product id is everything up to the second '&', i.e. vendor and product.
  pos = strchr(pos + 1, '&');
  if (!pos) {
    return;
  }
  const size_t bytesToCopy = static_cast<size_t>(pos - start);
  if (bytesToCopy >= aProductUniqueIdUTF8Length ||
      bytesToCopy > static_cast<size_t>(kVideoCaptureProductIdLength)) {
    RTC_LOG(LS_INFO) << "Failed to get the product Id";
    return;
  }
  memcpy(aProductUniqueIdUTF8, start, bytesToCopy);
  aProductUniqueIdUTF8[bytesToCopy] = '\0';
}

bool IsSameDeviceInstance(const nsACString& aIdA, const nsACString& aIdB) {
  return DeviceInstancePrefix(aIdA).Equals(DeviceInstancePrefix(aIdB),
                                           nsCaseInsensitiveCStringComparator);
}

HRESULT EnumerateCaptureDevices(nsTArray<MFCaptureDevice>& aDevices) {
  HRESULT hr = E_FAIL;
  RunInMTA([&] { hr = EnumerateCaptureDevicesMTA(aDevices); });
  return hr;
}

void ReleaseCaptureDevice(MFCaptureDevice& aDevice) {
  if (!aDevice.mActivate) {
    return;
  }
  RunInMTA([&] { aDevice.mActivate = nullptr; });
}

void ReleaseCaptureDevices(nsTArray<MFCaptureDevice>& aDevices) {
  if (aDevices.IsEmpty()) {
    return;
  }
  RunInMTA([&] { aDevices.Clear(); });
}

HRESULT GetDeviceCapabilities(IMFActivate* aActivate,
                              nsTArray<VideoCaptureCapability>& aCapabilities) {
  HRESULT hr = E_FAIL;
  RunInMTA([&] { hr = GetDeviceCapabilitiesMTA(aActivate, aCapabilities); });
  return hr;
}

bool CapabilityFromMediaType(IMFMediaType* aType,
                             VideoCaptureCapability& aCapability) {
  GUID subtype = GUID_NULL;
  if (FAILED(aType->GetGUID(MF_MT_SUBTYPE, &subtype))) {
    return false;
  }
  const VideoType videoType = VideoTypeFromMFSubtype(subtype);
  if (videoType == VideoType::kUnknown) {
    return false;
  }

  UINT32 width = 0;
  UINT32 height = 0;
  if (FAILED(MFGetAttributeSize(aType, MF_MT_FRAME_SIZE, &width, &height))) {
    return false;
  }
  constexpr UINT32 kMaxDimension = static_cast<UINT32>(INT32_MAX);
  if (width == 0 || height == 0 || width > kMaxDimension ||
      height > kMaxDimension) {
    return false;
  }

  // DirectShow reports the highest frame rate the device can produce for a
  // format, so prefer the range maximum and only fall back to the nominal rate.
  UINT32 numerator = 0;
  UINT32 denominator = 0;
  if (FAILED(MFGetAttributeRatio(aType, MF_MT_FRAME_RATE_RANGE_MAX, &numerator,
                                 &denominator)) ||
      denominator == 0) {
    if (FAILED(MFGetAttributeRatio(aType, MF_MT_FRAME_RATE, &numerator,
                                   &denominator)) ||
        denominator == 0) {
      numerator = 0;
      denominator = 1;
    }
  }

  const UINT32 interlaceMode = MFGetAttributeUINT32(
      aType, MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

  aCapability.width = static_cast<int32_t>(width);
  aCapability.height = static_cast<int32_t>(height);
  // Truncating rather than rounding matches DirectShow, which derives maxFPS
  // from an integer division of AvgTimePerFrame.
  aCapability.maxFPS = static_cast<int32_t>(numerator / denominator);
  aCapability.videoType = videoType;
  aCapability.interlaced =
      interlaceMode != static_cast<UINT32>(MFVideoInterlace_Progressive) &&
      interlaceMode != static_cast<UINT32>(MFVideoInterlace_Unknown);
  return true;
}

HRESULT CloneMediaTypeWithFrameRate(IMFMediaType* aType, int32_t aFPS,
                                    IMFMediaType** aOutType) {
  if (aFPS <= 0) {
    return E_INVALIDARG;
  }

  ComPtr<IMFMediaType> copy;
  HRESULT hr = mozilla::wmf::MFCreateMediaType(&copy);
  if (FAILED(hr)) {
    return hr;
  }
  hr = aType->CopyAllItems(copy.Get());
  if (FAILED(hr)) {
    return hr;
  }

  int32_t fps = aFPS;
  UINT32 numerator = 0;
  UINT32 denominator = 0;
  if (SUCCEEDED(MFGetAttributeRatio(aType, MF_MT_FRAME_RATE_RANGE_MIN,
                                    &numerator, &denominator)) &&
      denominator > 0) {
    fps = std::max(
        fps, static_cast<int32_t>((numerator + denominator - 1) / denominator));
  }
  if (SUCCEEDED(MFGetAttributeRatio(aType, MF_MT_FRAME_RATE_RANGE_MAX,
                                    &numerator, &denominator)) &&
      denominator > 0) {
    fps = std::min(fps, static_cast<int32_t>(numerator / denominator));
  }
  if (fps <= 0) {
    return E_INVALIDARG;
  }

  hr = MFSetAttributeRatio(copy.Get(), MF_MT_FRAME_RATE,
                           static_cast<UINT32>(fps), 1);
  if (FAILED(hr)) {
    return hr;
  }

  *aOutType = copy.Detach();
  return S_OK;
}

}  // namespace webrtc::videocapturemodule
