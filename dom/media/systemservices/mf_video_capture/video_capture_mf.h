/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_SYSTEMSERVICES_MF_VIDEO_CAPTURE_VIDEO_CAPTURE_MF_H_
#define DOM_MEDIA_SYSTEMSERVICES_MF_VIDEO_CAPTURE_VIDEO_CAPTURE_MF_H_

#include <wrl.h>

#include <atomic>

#include "PerformanceRecorder.h"
#include "api/scoped_refptr.h"
#include "api/sequence_checker.h"
#include "mf_capture_utils.h"
#include "modules/video_capture/video_capture_impl.h"
#include "mozilla/Maybe.h"
#include "mozilla/Monitor.h"
#include "rtc_base/synchronization/mutex.h"
#include "system_wrappers/include/clock.h"

namespace webrtc::videocapturemodule {

class VideoCaptureMF;

/**
 * Receives IMFSourceReader callbacks and forwards them to the VideoCaptureMF
 * that created it. Kept separate because webrtc::RefCountInterface and IUnknown
 * declare incompatible AddRef()/Release().
 *
 * The owner and reader pointers are raw, and every callback holds mMutex for
 * its whole duration. Detach() takes the same lock, so once it has returned no
 * callback can be running or start, and both may be torn down. Holding the
 * reader raw also avoids a reference cycle: the reader keeps a reference to
 * this callback.
 *
 * Re-arming ReadSample() lives here rather than in VideoCaptureMF so that the
 * reader is only ever touched under mMutex.
 */
class SourceReaderCallback final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<
              Microsoft::WRL::RuntimeClassType::ClassicCom>,
          IMFSourceReaderCallback> {
 public:
  HRESULT RuntimeClassInitialize(VideoCaptureMF* aOwner);

  IFACEMETHODIMP OnReadSample(HRESULT aStatus, DWORD aStreamIndex,
                              DWORD aStreamFlags, LONGLONG aTimestamp,
                              IMFSample* aSample) override;
  IFACEMETHODIMP OnFlush(DWORD aStreamIndex) override;
  IFACEMETHODIMP OnEvent(DWORD aStreamIndex, IMFMediaEvent* aEvent) override;

  void SetReader(IMFSourceReader* aReader);
  void Detach();

 private:
  Mutex mMutex;
  VideoCaptureMF* mOwner RTC_GUARDED_BY(mMutex) = nullptr;
  IMFSourceReader* mReader RTC_GUARDED_BY(mMutex) = nullptr;
};

/**
 * VideoCaptureImpl implementation of a Media Foundation camera backend.
 *
 * Single threaded, except for the IMFSourceReader callbacks that arrive on a
 * Media Foundation work queue thread.
 *
 * Frames are delivered through VideoCaptureImpl::IncomingFrame(), which
 * converts them to I420, exactly as the DirectShow backend's sink filter does.
 * The D3D11 texture path is not implemented; see the comment on
 * DeliverSoftwareFrame() for where it plugs in.
 */
class VideoCaptureMF : public VideoCaptureImpl {
 public:
  VideoCaptureMF(Clock* aClock, MFCaptureDevice&& aDevice);
  virtual ~VideoCaptureMF();

  static webrtc::scoped_refptr<VideoCaptureModule> Create(
      Clock* aClock, const char* aDeviceUniqueIdUTF8);

  // Implementation of VideoCaptureImpl. Single threaded.

  // Starts capturing synchronously. Idempotent. If a capture is live and a
  // different capability is requested, the source reader is torn down and
  // recreated.
  int32_t StartCapture(const VideoCaptureCapability& aCapability)
      MOZ_EXCLUDES(api_lock_) override;
  // Stops capturing synchronously. Idempotent.
  int32_t StopCapture() MOZ_EXCLUDES(api_lock_) override;
  bool CaptureStarted() MOZ_EXCLUDES(api_lock_) override;
  int32_t CaptureSettings(VideoCaptureCapability& aSettings)
      MOZ_EXCLUDES(api_lock_) override;

  void SetTrackingId(uint32_t aTrackingIdProcId)
      MOZ_EXCLUDES(api_lock_) override;

  // Callbacks. These are called on a Media Foundation work queue thread.
  // OnReadSample() returns whether another sample should be requested.
  bool OnReadSample(HRESULT aStatus, DWORD aStreamFlags, IMFSample* aSample)
      MOZ_EXCLUDES(api_lock_);
  void OnMediaTypeChanged(IMFMediaType* aMediaType) MOZ_EXCLUDES(api_lock_);
  void OnFlush();

 private:
  HRESULT OpenSourceReader();
  // Selects the native media type that best fits aCapability and makes it
  // current on the source reader. Fills aResult with what was negotiated.
  HRESULT SelectMediaType(const VideoCaptureCapability& aCapability,
                          VideoCaptureCapability& aResult);
  // Kicks off the read loop. Subsequent reads are requested by
  // SourceReaderCallback as each sample arrives.
  HRESULT RequestFirstSample();
  void Teardown();

  void DeliverSample(IMFSample* aSample) MOZ_EXCLUDES(api_lock_);
  void DeliverSoftwareFrame(IMFMediaBuffer* aBuffer,
                            const VideoCaptureCapability& aCapability)
      MOZ_EXCLUDES(api_lock_);

  // Registers the current thread with the profiler if not already registered.
  void MaybeRegisterCallbackThread();

  // Control thread checker.
  SequenceChecker mChecker;
  MFCaptureDevice mDevice RTC_GUARDED_BY(mChecker);
  Microsoft::WRL::ComPtr<IMFMediaSource> mSource RTC_GUARDED_BY(mChecker);
  Microsoft::WRL::ComPtr<IMFSourceReader> mReader RTC_GUARDED_BY(mChecker);
  Microsoft::WRL::ComPtr<SourceReaderCallback> mCallback
      RTC_GUARDED_BY(mChecker);
  // True while capture should keep pumping ReadSample calls. Read on the
  // callback thread.
  std::atomic<bool> mCapturing;
  // The capability StartCapture() was last called with, for idempotency.
  mozilla::Maybe<VideoCaptureCapability> mRequestedCapability
      MOZ_GUARDED_BY(api_lock_);
  // The capability negotiated with the device, describing the frames that
  // arrive in OnReadSample(). Nothing() while not capturing.
  mozilla::Maybe<VideoCaptureCapability> mCapability MOZ_GUARDED_BY(api_lock_);
  // Id string uniquely identifying this capture source.
  mozilla::Maybe<mozilla::TrackingId> mTrackingId MOZ_GUARDED_BY(api_lock_);
  // Adds frame specific markers to the profiler while mTrackingId is set.
  // Callback thread only.
  mozilla::PerformanceRecorderMulti<mozilla::CaptureStage> mCaptureRecorder;
  std::atomic<ProfilerThreadId> mCallbackThreadId;
  // Signalled by OnFlush() to let StopCapture() know the reader has drained.
  mozilla::Monitor mFlushMonitor MOZ_UNANNOTATED;
  bool mFlushed = false;
};

}  // namespace webrtc::videocapturemodule

#endif
