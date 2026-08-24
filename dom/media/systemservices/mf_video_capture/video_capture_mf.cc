/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "video_capture_mf.h"

#include <cstring>
#include <utility>
#include <vector>

#include "CallbackThreadRegistry.h"
#include "api/make_ref_counted.h"
#include "modules/video_capture/video_capture_defines.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/mscom/EnsureMTA.h"
#include "rtc_base/logging.h"
#include "rtc_base/string_utils.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::MakeAndInitialize;

namespace webrtc::videocapturemodule {

namespace {

constexpr DWORD kVideoStream =
    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

// StopCapture() must not hang if Media Foundation never reports the flush as
// complete. Detaching the callback makes a late callback a no-op anyway.
constexpr uint32_t kFlushTimeoutMs = 2000;

mozilla::CaptureStage::ImageType ToImageType(VideoType aType) {
  switch (aType) {
    case VideoType::kI420:
      return mozilla::CaptureStage::ImageType::I420;
    case VideoType::kYUY2:
      return mozilla::CaptureStage::ImageType::YUY2;
    case VideoType::kYV12:
    case VideoType::kIYUV:
      return mozilla::CaptureStage::ImageType::YV12;
    case VideoType::kUYVY:
      return mozilla::CaptureStage::ImageType::UYVY;
    case VideoType::kNV12:
      return mozilla::CaptureStage::ImageType::NV12;
    case VideoType::kNV21:
      return mozilla::CaptureStage::ImageType::NV21;
    case VideoType::kMJPEG:
      return mozilla::CaptureStage::ImageType::MJPEG;
    default:
      return mozilla::CaptureStage::ImageType::Unknown;
  }
}

// Must run in the multithreaded apartment. On success aActivate's media source
// is live and must eventually be shut down with IMFActivate::ShutdownObject().
HRESULT OpenSourceReaderMTA(IMFActivate* aActivate,
                            SourceReaderCallback* aCallback,
                            ComPtr<IMFMediaSource>& aOutSource,
                            ComPtr<IMFSourceReader>& aOutReader) {
  // Activation failure is reported as-is rather than retried here. Camera
  // shutdown can be asynchronous, so an open can fail transiently, but retrying
  // belongs above the backend: libwebrtc's CameraCapturer already does it for
  // Android with MAX_OPEN_CAMERA_ATTEMPTS and a delay between attempts. A retry
  // in here would also mask a camera that is genuinely in use by another
  // application, which is the failure bug 2036382 needs surfaced.
  ComPtr<IMFMediaSource> source;
  HRESULT hr = aActivate->ActivateObject(IID_PPV_ARGS(&source));
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "ActivateObject failed, 0x" << ToHex(hr);
    return hr;
  }

  bool succeeded = false;
  auto shutdown = mozilla::MakeScopeExit([&] {
    if (!succeeded) {
      source = nullptr;
      (void)aActivate->ShutdownObject();
    }
  });

  ComPtr<IMFAttributes> attributes;
  hr = mozilla::wmf::MFCreateAttributes(&attributes, 1);
  if (FAILED(hr)) {
    return hr;
  }
  hr = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, aCallback);
  if (FAILED(hr)) {
    return hr;
  }

  // MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING is deliberately not set.
  // It inserts a Video Processor MFT, which would add exactly the conversion
  // and copy that this backend exists to avoid; capability negotiation already
  // restricts us to formats the device produces natively.
  //
  // Zero-copy seam: getting D3D11 textures out of the camera instead of system
  // memory is a matter of creating an IMFDXGIDeviceManager over a device from
  // gfx::DeviceManagerDx (with ID3D10Multithread::SetMultithreadProtected) and
  // adding it here:
  //   attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager);
  //   attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
  // Samples then carry an IMFDXGIBuffer, handled in DeliverSample(). Which
  // D3D11 device to pick depends on which process capture runs in, since gecko
  // only forwards a texture without a readback when the producing and consuming
  // processes match.

  ComPtr<IMFSourceReader> reader;
  hr = MFCreateSourceReaderFromMediaSourceShim(source.Get(), attributes.Get(),
                                               &reader);
  if (FAILED(hr)) {
    return hr;
  }

  succeeded = true;
  aOutSource = std::move(source);
  aOutReader = std::move(reader);
  return S_OK;
}

// Must run in the multithreaded apartment.
HRESULT SelectMediaTypeMTA(IMFSourceReader* aReader,
                           const VideoCaptureCapability& aCapability,
                           VideoCaptureCapability& aResult) {
  // The requested capability comes from what this backend's DeviceInfo handed
  // out, so an exact match should always exist. The looser fallbacks are there
  // so that a capability that has been adjusted on the way down still starts
  // capture rather than failing outright.
  ComPtr<IMFMediaType> exact;
  ComPtr<IMFMediaType> sameFormat;
  ComPtr<IMFMediaType> sameSize;
  VideoCaptureCapability exactCapability;
  VideoCaptureCapability sameFormatCapability;
  VideoCaptureCapability sameSizeCapability;

  for (DWORD index = 0;; ++index) {
    ComPtr<IMFMediaType> type;
    HRESULT hr = aReader->GetNativeMediaType(kVideoStream, index, &type);
    if (hr == MF_E_NO_MORE_TYPES) {
      break;
    }
    if (FAILED(hr)) {
      return hr;
    }

    VideoCaptureCapability candidate;
    if (!CapabilityFromMediaType(type.Get(), candidate)) {
      continue;
    }
    if (candidate.width != aCapability.width ||
        candidate.height != aCapability.height) {
      continue;
    }
    if (!sameSize) {
      sameSize = type;
      sameSizeCapability = candidate;
    }
    if (candidate.videoType != aCapability.videoType) {
      continue;
    }
    if (!sameFormat) {
      sameFormat = type;
      sameFormatCapability = candidate;
    }
    if (candidate.maxFPS != aCapability.maxFPS) {
      continue;
    }
    exact = type;
    exactCapability = candidate;
    break;
  }

  ComPtr<IMFMediaType> selected;
  VideoCaptureCapability selectedCapability;
  if (exact) {
    selected = exact;
    selectedCapability = exactCapability;
  } else if (sameFormat) {
    selected = sameFormat;
    selectedCapability = sameFormatCapability;
  } else if (sameSize) {
    selected = sameSize;
    selectedCapability = sameSizeCapability;
  } else {
    RTC_LOG(LS_ERROR) << "No native media type matches " << aCapability.width
                      << "x" << aCapability.height;
    return MF_E_INVALIDMEDIATYPE;
  }

  int32_t fps =
      aCapability.maxFPS > 0 ? aCapability.maxFPS : selectedCapability.maxFPS;

  HRESULT hr = E_FAIL;
  ComPtr<IMFMediaType> adjusted;
  if (fps > 0 &&
      SUCCEEDED(CloneMediaTypeWithFrameRate(selected.Get(), fps, &adjusted))) {
    hr = aReader->SetCurrentMediaType(kVideoStream, nullptr, adjusted.Get());
  }
  if (FAILED(hr)) {
    // Not every camera accepts a frame rate other than the format's default.
    hr = aReader->SetCurrentMediaType(kVideoStream, nullptr, selected.Get());
    if (FAILED(hr)) {
      return hr;
    }
    fps = selectedCapability.maxFPS;
  }

  ComPtr<IMFMediaType> current;
  hr = aReader->GetCurrentMediaType(kVideoStream, &current);
  if (FAILED(hr)) {
    return hr;
  }
  if (!CapabilityFromMediaType(current.Get(), aResult)) {
    return MF_E_INVALIDMEDIATYPE;
  }
  aResult.maxFPS = fps;

  RTC_LOG(LS_INFO) << "Capturing " << aResult.width << "x" << aResult.height
                   << "@" << aResult.maxFPS
                   << " type:" << static_cast<int>(aResult.videoType);
  return S_OK;
}

}  // namespace

HRESULT SourceReaderCallback::RuntimeClassInitialize(VideoCaptureMF* aOwner) {
  MutexLock lock(&mMutex);
  mOwner = aOwner;
  return S_OK;
}

void SourceReaderCallback::SetReader(IMFSourceReader* aReader) {
  MutexLock lock(&mMutex);
  mReader = aReader;
}

void SourceReaderCallback::Detach() {
  MutexLock lock(&mMutex);
  mOwner = nullptr;
  mReader = nullptr;
}

IFACEMETHODIMP SourceReaderCallback::OnReadSample(HRESULT aStatus,
                                                  DWORD /* aStreamIndex */,
                                                  DWORD aStreamFlags,
                                                  LONGLONG /* aTimestamp */,
                                                  IMFSample* aSample) {
  MutexLock lock(&mMutex);
  if (!mOwner || !mReader) {
    return S_OK;
  }

  if (aStreamFlags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
    ComPtr<IMFMediaType> current;
    if (SUCCEEDED(mReader->GetCurrentMediaType(kVideoStream, &current))) {
      mOwner->OnMediaTypeChanged(current.Get());
    }
  }

  if (!mOwner->OnReadSample(aStatus, aStreamFlags, aSample)) {
    return S_OK;
  }

  HRESULT hr =
      mReader->ReadSample(kVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "ReadSample failed, 0x" << ToHex(hr);
  }
  return S_OK;
}

IFACEMETHODIMP SourceReaderCallback::OnFlush(DWORD /* aStreamIndex */) {
  MutexLock lock(&mMutex);
  if (mOwner) {
    mOwner->OnFlush();
  }
  return S_OK;
}

IFACEMETHODIMP SourceReaderCallback::OnEvent(DWORD /* aStreamIndex */,
                                             IMFMediaEvent* /* aEvent */) {
  return S_OK;
}

VideoCaptureMF::VideoCaptureMF(Clock* aClock, MFCaptureDevice&& aDevice)
    : VideoCaptureImpl(aClock),
      mDevice(std::move(aDevice)),
      mCapturing(false),
      mCallbackThreadId(),
      mFlushMonitor("VideoCaptureMF::mFlushMonitor") {
  const size_t length = mDevice.mUniqueId.Length();
  _deviceUniqueId = new (std::nothrow) char[length + 1];
  if (_deviceUniqueId) {
    memcpy(_deviceUniqueId, mDevice.mUniqueId.get(), length + 1);
  }
}

VideoCaptureMF::~VideoCaptureMF() {
  RTC_DCHECK_RUN_ON(&mChecker);
  // Must block until capture has fully stopped, including the async flush.
  StopCapture();
  ReleaseCaptureDevice(mDevice);
}

/* static */
webrtc::scoped_refptr<VideoCaptureModule> VideoCaptureMF::Create(
    Clock* aClock, const char* aDeviceUniqueIdUTF8) {
  if (!aDeviceUniqueIdUTF8 || !aDeviceUniqueIdUTF8[0]) {
    return nullptr;
  }
  if (strlen(aDeviceUniqueIdUTF8) >= kVideoCaptureUniqueNameLength) {
    return nullptr;
  }

  nsTArray<MFCaptureDevice> devices;
  auto release =
      mozilla::MakeScopeExit([&] { ReleaseCaptureDevices(devices); });

  if (FAILED(EnumerateCaptureDevices(devices))) {
    return nullptr;
  }
  for (auto& device : devices) {
    if (device.mUniqueId.Equals(nsDependentCString(aDeviceUniqueIdUTF8),
                                nsCaseInsensitiveCStringComparator)) {
      return webrtc::make_ref_counted<VideoCaptureMF>(aClock,
                                                      std::move(device));
    }
  }

  RTC_LOG(LS_ERROR) << "No Media Foundation capture device matches the "
                       "requested id";
  return nullptr;
}

int32_t VideoCaptureMF::StartCapture(
    const VideoCaptureCapability& aCapability) {
  RTC_DCHECK_RUN_ON(&mChecker);

  {
    MutexLock lock(&api_lock_);
    if (mRequestedCapability && *mRequestedCapability == aCapability) {
      return 0;
    }
  }

  if (mReader) {
    if (int32_t rv = StopCapture(); rv != 0) {
      return rv;
    }
  }

  HRESULT hr = OpenSourceReader();
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Failed to open the capture source reader, 0x"
                      << ToHex(hr);
    Teardown();
    return -1;
  }

  VideoCaptureCapability negotiated;
  hr = SelectMediaType(aCapability, negotiated);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Failed to select a capture media type, 0x"
                      << ToHex(hr);
    Teardown();
    return -1;
  }

  {
    MutexLock lock(&api_lock_);
    mRequestedCapability = mozilla::Some(aCapability);
    mCapability = mozilla::Some(negotiated);
  }
  mCapturing = true;

  hr = RequestFirstSample();
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Initial ReadSample failed, 0x" << ToHex(hr);
    mCapturing = false;
    Teardown();
    MutexLock lock(&api_lock_);
    mRequestedCapability = mozilla::Nothing();
    mCapability = mozilla::Nothing();
    return -1;
  }

  return 0;
}

int32_t VideoCaptureMF::StopCapture() {
  RTC_DCHECK_RUN_ON(&mChecker);

  if (!mReader) {
    return 0;
  }
  mCapturing = false;

  {
    mozilla::MonitorAutoLock lock(mFlushMonitor);
    mFlushed = false;
  }

  IMFSourceReader* reader = mReader.Get();
  HRESULT hr = E_FAIL;
  mozilla::mscom::EnsureMTA([&] { hr = reader->Flush(kVideoStream); });

  if (SUCCEEDED(hr)) {
    mozilla::MonitorAutoLock lock(mFlushMonitor);
    const mozilla::TimeStamp deadline =
        mozilla::TimeStamp::Now() +
        mozilla::TimeDuration::FromMilliseconds(kFlushTimeoutMs);
    while (!mFlushed) {
      const mozilla::TimeDuration remaining =
          deadline - mozilla::TimeStamp::Now();
      if (remaining <= mozilla::TimeDuration()) {
        RTC_LOG(LS_WARNING)
            << "Timed out waiting for the capture source reader to flush";
        break;
      }
      lock.Wait(remaining);
    }
  } else {
    RTC_LOG(LS_WARNING) << "Failed to flush the capture source reader, 0x"
                        << ToHex(hr);
  }

  Teardown();

  MutexLock lock(&api_lock_);
  mRequestedCapability = mozilla::Nothing();
  mCapability = mozilla::Nothing();
  return 0;
}

bool VideoCaptureMF::CaptureStarted() {
  RTC_DCHECK_RUN_ON(&mChecker);
  MutexLock lock(&api_lock_);
  return mCapability.isSome();
}

int32_t VideoCaptureMF::CaptureSettings(VideoCaptureCapability& aSettings) {
  MutexLock lock(&api_lock_);
  if (!mCapability) {
    return -1;
  }
  aSettings = *mCapability;
  return 0;
}

void VideoCaptureMF::SetTrackingId(uint32_t aTrackingIdProcId) {
  RTC_DCHECK_RUN_ON(&mChecker);
  MutexLock lock(&api_lock_);
  if (NS_WARN_IF(mTrackingId.isSome())) {
    // This capture instance must be shared across multiple camera requests. For
    // now ignore other requests than the first.
    return;
  }
  mTrackingId.emplace(mozilla::TrackingId::Source::Camera, aTrackingIdProcId);
}

HRESULT VideoCaptureMF::OpenSourceReader() {
  RTC_DCHECK_RUN_ON(&mChecker);
  MOZ_ASSERT(!mReader);

  ComPtr<SourceReaderCallback> callback;
  HRESULT hr = MakeAndInitialize<SourceReaderCallback>(&callback, this);
  if (FAILED(hr)) {
    return hr;
  }

  // Read into a raw pointer so that the lambda does not have to satisfy the
  // SequenceChecker annotation on mDevice, as Teardown() does.
  IMFActivate* activate = mDevice.mActivate.Get();
  ComPtr<IMFMediaSource> source;
  ComPtr<IMFSourceReader> reader;
  mozilla::mscom::EnsureMTA([&] {
    hr = OpenSourceReaderMTA(activate, callback.Get(), source, reader);
  });
  if (FAILED(hr)) {
    return hr;
  }

  callback->SetReader(reader.Get());
  mCallback = std::move(callback);
  mSource = std::move(source);
  mReader = std::move(reader);
  return S_OK;
}

HRESULT VideoCaptureMF::SelectMediaType(
    const VideoCaptureCapability& aCapability,
    VideoCaptureCapability& aResult) {
  RTC_DCHECK_RUN_ON(&mChecker);

  IMFSourceReader* reader = mReader.Get();
  HRESULT hr = E_FAIL;
  mozilla::mscom::EnsureMTA(
      [&] { hr = SelectMediaTypeMTA(reader, aCapability, aResult); });
  return hr;
}

HRESULT VideoCaptureMF::RequestFirstSample() {
  RTC_DCHECK_RUN_ON(&mChecker);

  IMFSourceReader* reader = mReader.Get();
  HRESULT hr = E_FAIL;
  mozilla::mscom::EnsureMTA([&] {
    hr =
        reader->ReadSample(kVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
  });
  return hr;
}

void VideoCaptureMF::Teardown() {
  RTC_DCHECK_RUN_ON(&mChecker);

  ComPtr<SourceReaderCallback> callback = std::move(mCallback);
  ComPtr<IMFSourceReader> reader = std::move(mReader);
  ComPtr<IMFMediaSource> source = std::move(mSource);
  IMFActivate* activate = mDevice.mActivate.Get();

  if (callback) {
    // Blocks until any in-flight callback has returned, after which none can
    // reach this instance or the reader.
    callback->Detach();
  }

  mozilla::mscom::EnsureMTA([&] {
    reader = nullptr;
    if (source) {
      source = nullptr;
      (void)activate->ShutdownObject();
    }
    callback = nullptr;
  });
}

bool VideoCaptureMF::OnReadSample(HRESULT aStatus, DWORD aStreamFlags,
                                  IMFSample* aSample) {
  MaybeRegisterCallbackThread();

  if (FAILED(aStatus)) {
    RTC_LOG(LS_ERROR) << "Capture source reader failed, 0x" << ToHex(aStatus);
    return false;
  }
  if (aStreamFlags & MF_SOURCE_READERF_ERROR) {
    RTC_LOG(LS_ERROR) << "Capture source reader signalled an error";
    return false;
  }
  if (aStreamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
    RTC_LOG(LS_INFO) << "Capture stream ended";
    return false;
  }

  if (aSample) {
    DeliverSample(aSample);
  }

  return mCapturing;
}

void VideoCaptureMF::OnMediaTypeChanged(IMFMediaType* aMediaType) {
  VideoCaptureCapability capability;
  if (!CapabilityFromMediaType(aMediaType, capability)) {
    RTC_LOG(LS_ERROR) << "Capture media type changed to one we cannot deliver";
    MutexLock lock(&api_lock_);
    mCapability = mozilla::Nothing();
    return;
  }

  MutexLock lock(&api_lock_);
  if (mCapability) {
    // The frame rate the device settled on is not renegotiated here.
    capability.maxFPS = mCapability->maxFPS;
  }
  RTC_LOG(LS_WARNING) << "Capture media type changed to " << capability.width
                      << "x" << capability.height
                      << " type:" << static_cast<int>(capability.videoType);
  mCapability = mozilla::Some(capability);
}

void VideoCaptureMF::OnFlush() {
  mozilla::MonitorAutoLock lock(mFlushMonitor);
  mFlushed = true;
  lock.Notify();
}

void VideoCaptureMF::DeliverSample(IMFSample* aSample) {
  VideoCaptureCapability capability;
  {
    MutexLock lock(&api_lock_);
    if (!mCapability) {
      return;
    }
    capability = *mCapability;
  }

  ComPtr<IMFMediaBuffer> buffer;
  // Camera samples carry a single buffer, so this does not copy in practice; it
  // only does for the unexpected multi-buffer case.
  HRESULT hr = aSample->ConvertToContiguousBuffer(&buffer);
  if (FAILED(hr) || !buffer) {
    RTC_LOG(LS_ERROR) << "ConvertToContiguousBuffer failed, 0x" << ToHex(hr);
    return;
  }

  // Zero-copy seam: once MF_SOURCE_READER_D3D_MANAGER is set in
  // OpenSourceReaderMTA(), buffer exposes IMFDXGIBuffer and holds an
  // ID3D11Texture2D. That case belongs here, as a DeliverTextureFrame() that
  // wraps the texture in a gecko Image and passes it on without touching the
  // pixels, instead of falling through to the lock and I420 conversion below.
  DeliverSoftwareFrame(buffer.Get(), capability);
}

void VideoCaptureMF::DeliverSoftwareFrame(
    IMFMediaBuffer* aBuffer, const VideoCaptureCapability& aCapability) {
  ComPtr<IMF2DBuffer2> buffer2D;
  BYTE* scanline0 = nullptr;
  LONG pitch = 0;
  BYTE* bufferStart = nullptr;
  DWORD length = 0;

  if (FAILED(aBuffer->QueryInterface(IID_PPV_ARGS(&buffer2D))) ||
      FAILED(buffer2D->Lock2DSize(MF2DBuffer_LockFlags_Read, &scanline0, &pitch,
                                  &bufferStart, &length))) {
    buffer2D = nullptr;
    DWORD maxLength = 0;
    HRESULT hr = aBuffer->Lock(&bufferStart, &maxLength, &length);
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "Failed to lock the capture buffer, 0x" << ToHex(hr);
      return;
    }
    scanline0 = bufferStart;
    // 0 makes ConvertToI420() derive the stride from the width and format.
    pitch = 0;
  }
  auto unlock = mozilla::MakeScopeExit([&] {
    if (buffer2D) {
      buffer2D->Unlock2D();
    } else {
      aBuffer->Unlock();
    }
  });

  VideoCaptureCapability frameInfo = aCapability;
  BYTE* data = scanline0;
  int32_t stride = pitch;
  if (pitch < 0) {
    // A negative pitch means the rows are stored bottom-up. Pass the start of
    // the buffer and a negative height, which makes libyuv flip while
    // converting.
    data = bufferStart;
    stride = -pitch;
    frameInfo.height = -frameInfo.height;
  }

  if (MutexLock lock(&api_lock_); MOZ_LIKELY(mTrackingId)) {
    mCaptureRecorder.Start(
        0, "VideoCaptureMediaFoundation"_ns, *mTrackingId, aCapability.width,
        aCapability.height,
        ToImageType(aCapability.videoType));
  }

  SetStride(stride);
  IncomingFrame(data, length, frameInfo);
  mCaptureRecorder.Record(0);
}

void VideoCaptureMF::MaybeRegisterCallbackThread() {
  ProfilerThreadId id = profiler_current_thread_id();
  if (MOZ_LIKELY(id == mCallbackThreadId)) {
    return;
  }
  mCallbackThreadId = id;
  mozilla::CallbackThreadRegistry::Get()->Register(
      mCallbackThreadId, "VideoCaptureMediaFoundationCallback");
}

}  // namespace webrtc::videocapturemodule
