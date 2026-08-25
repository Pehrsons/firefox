/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_SYSTEMSERVICES_MF_VIDEO_CAPTURE_MF_CAPTURE_UTILS_H_
#define DOM_MEDIA_SYSTEMSERVICES_MF_VIDEO_CAPTURE_MF_CAPTURE_UTILS_H_

#include <wrl.h>

#include <vector>

#include "WMF.h"

// WMF.h reaches Shlwapi.h through propvarutil.h, and its StrCat macro would
// otherwise rewrite absl::StrCat in rtc_base/logging.h to absl::StrCatW. See
// third_party/libwebrtc/moz-patch-stack/s0113.patch for the same collision.
#undef StrCat

#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/video_capture/video_capture_defines.h"
#include "nsString.h"
#include "nsTArray.h"

namespace webrtc::videocapturemodule {

/**
 * Media Foundation entry points that mozilla::wmf does not wrap yet. They are
 * loaded here, with the same GetProcAddress-based approach as
 * dom/media/platforms/wmf/WMFUtils.cpp, so that no MF import library has to be
 * added to xul. Neither mf.dll nor mfreadwrite.dll is loaded anywhere else.
 */
HRESULT MFEnumDeviceSourcesShim(IMFAttributes* aAttributes,
                                IMFActivate*** aSources, UINT32* aCount);
HRESULT MFCreateSourceReaderFromMediaSourceShim(IMFMediaSource* aSource,
                                                IMFAttributes* aAttributes,
                                                IMFSourceReader** aReader);

/**
 * True if the Media Foundation camera backend is enabled by pref and Media
 * Foundation could be started in this process.
 */
bool MediaFoundationCaptureEnabled();

/**
 * Maps a Media Foundation video subtype to the VideoType that
 * VideoCaptureImpl::IncomingFrame() can convert to I420. Returns
 * VideoType::kUnknown for anything else, which callers must skip.
 *
 * The accepted set is deliberately the DirectShow backend's set plus NV12.
 * Notably MFVideoFormat_RGB32 is left out: DirectShow does not offer RGB32
 * capabilities either, and adding a packed RGB format would let
 * DeviceInfoImpl::GetBestMatchedCapability() select it over a YUV format,
 * making conversion more expensive rather than less.
 */
VideoType VideoTypeFromMFSubtype(const GUID& aSubtype);

/**
 * Extracts vendor and product from a Media Foundation symbolic link into
 * aProductUniqueIdUTF8, in the same format DeviceInfoDS::GetProductId()
 * produces from a DirectShow DevicePath. Both strings have the shape
 * "\\?\usb#vid_0408&pid_2010&mi_00#...".
 */
void GetProductId(const char* aSymbolicLink, char* aProductUniqueIdUTF8,
                  uint32_t aProductUniqueIdUTF8Length);

/**
 * True if the two device ids name the same underlying device instance. A Media
 * Foundation symbolic link and a DirectShow DevicePath for one camera are
 * identical except for the trailing device interface class GUID, so comparing
 * everything before that GUID is what makes ids from the two backends
 * comparable. Ids with no GUID at all, which is what DeviceInfoDS falls back to
 * for software filters that have no DevicePath, only match themselves.
 */
bool IsSameDeviceInstance(const nsACString& aIdA, const nsACString& aIdB);

/**
 * A camera as reported by MFEnumDeviceSources. mUniqueId is the Media
 * Foundation symbolic link, which serves the same purpose as the DirectShow
 * DevicePath: a stable per-device identifier. Note that the two are not
 * interchangeable -- they end in different device interface class GUIDs.
 */
struct MFCaptureDevice {
  nsCString mUniqueId;
  nsCString mName;
  Microsoft::WRL::ComPtr<IMFActivate> mActivate;
};

/**
 * Enumerates video capture devices. Safe to call from any thread; the Media
 * Foundation calls are made in the multithreaded apartment.
 */
HRESULT EnumerateCaptureDevices(nsTArray<MFCaptureDevice>& aDevices);

/**
 * Drops the COM references held by the given devices from the multithreaded
 * apartment. Callers that own MFCaptureDevices on a thread with no apartment of
 * its own should use this rather than letting the destructor run there.
 */
void ReleaseCaptureDevice(MFCaptureDevice& aDevice);
void ReleaseCaptureDevices(nsTArray<MFCaptureDevice>& aDevices);

/**
 * Activates aActivate and appends the native media types of its first video
 * stream to aCapabilities. The activated media source is shut down again before
 * returning, so this opens and closes the device.
 */
HRESULT GetDeviceCapabilities(IMFActivate* aActivate,
                              nsTArray<VideoCaptureCapability>& aCapabilities);

/**
 * Reads width, height, frame rate and video type off aType. Returns false if
 * aType is not a video type this backend can deliver.
 */
bool CapabilityFromMediaType(IMFMediaType* aType,
                             VideoCaptureCapability& aCapability);

/**
 * Creates a copy of aType with MF_MT_FRAME_RATE set to aFPS, clamped into the
 * type's MF_MT_FRAME_RATE_RANGE_MIN..MAX if it advertises one. Used to ask a
 * camera for a lower frame rate than the format's maximum, the way
 * VideoCaptureDS does by rewriting AvgTimePerFrame. Must be called in the
 * multithreaded apartment.
 */
HRESULT CloneMediaTypeWithFrameRate(IMFMediaType* aType, int32_t aFPS,
                                    IMFMediaType** aOutType);

}  // namespace webrtc::videocapturemodule

#endif
