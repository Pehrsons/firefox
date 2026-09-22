/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CanvasCaptureMediaStream.h"

#include "DOMMediaStream.h"
#include "ImageContainer.h"
#include "MediaTrackGraph.h"
#include "Tracing.h"
#include "VideoSegment.h"
#include "mozilla/dom/CanvasCaptureMediaStreamBinding.h"
#include "mozilla/gfx/2D.h"
#include "nsContentUtils.h"
#include "nsGlobalWindowInner.h"

using namespace mozilla::layers;
using namespace mozilla::gfx;

namespace mozilla::dom {

static uint32_t sCaptureSourceId = 0;

NS_IMPL_ADDREF_INHERITED(CanvasCaptureTrackSource, MediaStreamTrackSource)
NS_IMPL_RELEASE_INHERITED(CanvasCaptureTrackSource, MediaStreamTrackSource)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(CanvasCaptureTrackSource)
NS_INTERFACE_MAP_END_INHERITING(MediaStreamTrackSource)
NS_IMPL_CYCLE_COLLECTION_CLASS(CanvasCaptureTrackSource)
NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(CanvasCaptureTrackSource,
                                                MediaStreamTrackSource)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mCaptureStream)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_WEAK_PTR
NS_IMPL_CYCLE_COLLECTION_UNLINK_END
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(CanvasCaptureTrackSource,
                                                  MediaStreamTrackSource)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mCaptureStream)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

CanvasCaptureTrackSource::CanvasCaptureTrackSource(
    nsIPrincipal* aPrincipal, CanvasCaptureMediaStream* aCaptureStream)
    : MediaStreamTrackSource(
          aPrincipal, nsString(),
          TrackingId(TrackingId::Source::Canvas, sCaptureSourceId++,
                     TrackingId::TrackAcrossProcesses::Yes)),
      mCaptureStream(aCaptureStream) {
  HTMLCanvasElement* canvas = mCaptureStream->Canvas();
  SetCanvasSize(CSSIntSize(canvas->Width(), canvas->Height()));
}

bool CanvasCaptureTrackSource::HasAlpha() const {
  if (!mCaptureStream || !mCaptureStream->Canvas()) {
    // In cycle-collection
    return false;
  }
  return !mCaptureStream->Canvas()->GetIsOpaque();
}

void CanvasCaptureTrackSource::Stop() {
  if (!mCaptureStream) {
    return;
  }

  mCaptureStream->StopCapture();
}

void CanvasCaptureTrackSource::SetCanvasSize(const CSSIntSize& aSize) {
  MOZ_ASSERT(NS_IsMainThread());
  mSettings = MediaStreamTrackSourceSettings{.mWidth = Some(aSize.width),
                                             .mHeight = Some(aSize.height)};
}

OutputStreamDriver::OutputStreamDriver(SourceMediaTrack* aSourceStream,
                                       const PrincipalHandle& aPrincipalHandle,
                                       CanvasCaptureTrackSource* aTrackSource)
    : mSourceStream(aSourceStream),
      mPrincipalHandle(aPrincipalHandle),
      mTrackSource(aTrackSource) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mSourceStream);
}

OutputStreamDriver::~OutputStreamDriver() {
  MOZ_ASSERT(NS_IsMainThread());
  EndTrack();
}

void OutputStreamDriver::CanvasSizeChanged(const CSSIntSize& aSize) {
  MOZ_ASSERT(NS_IsMainThread());
  if (mTrackSource) {
    mTrackSource->SetCanvasSize(aSize);
  }
}

void OutputStreamDriver::EndTrack() {
  MOZ_ASSERT(NS_IsMainThread());
  if (!mSourceStream->IsDestroyed()) {
    mSourceStream->Destroy();
  }
}

void OutputStreamDriver::SetImage(RefPtr<layers::Image>&& aImage,
                                  const TimeStamp& aTime) {
  MOZ_ASSERT(NS_IsMainThread());

  VideoSegment segment;
  const auto size = aImage->GetSize();
  segment.AppendFrame(aImage.forget(), size, mPrincipalHandle, false, aTime);
  mSourceStream->AppendData(&segment);
}

// ----------------------------------------------------------------------

class TimerDriver : public OutputStreamDriver {
 public:
  TimerDriver(SourceMediaTrack* aSourceStream, const double& aFPS,
              const PrincipalHandle& aPrincipalHandle,
              CanvasCaptureTrackSource* aTrackSource)
      : OutputStreamDriver(aSourceStream, aPrincipalHandle, aTrackSource),
        mFrameInterval(aFPS == 0.0 ? TimeDuration::Forever()
                                   : TimeDuration::FromSeconds(1.0 / aFPS)) {}

  void RequestFrameCapture() override { mExplicitCaptureRequested = true; }

  bool FrameCaptureRequested(const TimeStamp& aTime) const override {
    if (mLastFrameTime.IsNull()) {
      // All CanvasCaptureMediaStreams shall at least get one frame.
      return true;
    }

    if (mExplicitCaptureRequested) {
      return true;
    }

    if ((aTime - mLastFrameTime) >= mFrameInterval) {
      return true;
    }

    return false;
  }

  void NewFrame(already_AddRefed<Image> aImage,
                const TimeStamp& aTime) override {
    nsCString str;
    if (profiler_thread_is_being_profiled_for_markers()) {
      TimeDuration sinceLast =
          aTime - (mLastFrameTime.IsNull() ? aTime : mLastFrameTime);
      str.AppendPrintf(
          "TimerDriver %staking frame (%sexplicitly requested; after %.2fms; "
          "interval cap %.2fms)",
          sinceLast >= mFrameInterval ? "" : "NOT ",
          mExplicitCaptureRequested ? "" : "NOT ", sinceLast.ToMilliseconds(),
          mFrameInterval.ToMilliseconds());
    }
    AUTO_PROFILER_MARKER_TEXT("Canvas CaptureStream", MEDIA_RT, {}, str);

    RefPtr<Image> image = aImage;

    if (!FrameCaptureRequested(aTime)) {
      return;
    }

    mLastFrameTime = aTime;
    mExplicitCaptureRequested = false;
    SetImage(std::move(image), aTime);
  }

 protected:
  virtual ~TimerDriver() = default;

 private:
  const TimeDuration mFrameInterval;
  bool mExplicitCaptureRequested = false;
  TimeStamp mLastFrameTime;
};

// ----------------------------------------------------------------------

class AutoDriver : public OutputStreamDriver {
 public:
  AutoDriver(SourceMediaTrack* aSourceStream,
             const PrincipalHandle& aPrincipalHandle,
             CanvasCaptureTrackSource* aTrackSource)
      : OutputStreamDriver(aSourceStream, aPrincipalHandle, aTrackSource) {}

  void RequestFrameCapture() override {}

  bool FrameCaptureRequested(const TimeStamp& aTime) const override {
    return true;
  }

  void NewFrame(already_AddRefed<Image> aImage,
                const TimeStamp& aTime) override {
    AUTO_PROFILER_MARKER_TEXT("Canvas CaptureStream", MEDIA_RT, {},
                              "AutoDriver taking frame"_ns);

    RefPtr<Image> image = aImage;
    SetImage(std::move(image), aTime);
  }

 protected:
  virtual ~AutoDriver() = default;
};

// ----------------------------------------------------------------------

NS_IMPL_CYCLE_COLLECTION_INHERITED(CanvasCaptureMediaStream, DOMMediaStream,
                                   mCanvas)

NS_IMPL_ADDREF_INHERITED(CanvasCaptureMediaStream, DOMMediaStream)
NS_IMPL_RELEASE_INHERITED(CanvasCaptureMediaStream, DOMMediaStream)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(CanvasCaptureMediaStream)
NS_INTERFACE_MAP_END_INHERITING(DOMMediaStream)

CanvasCaptureMediaStream::CanvasCaptureMediaStream(nsIGlobalObject* aGlobal,
                                                   HTMLCanvasElement* aCanvas)
    : DOMMediaStream(aGlobal), mCanvas(aCanvas) {}

CanvasCaptureMediaStream::~CanvasCaptureMediaStream() = default;

JSObject* CanvasCaptureMediaStream::WrapObject(
    JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return dom::CanvasCaptureMediaStream_Binding::Wrap(aCx, this, aGivenProto);
}

void CanvasCaptureMediaStream::RequestFrame() {
  if (mOutputStreamDriver) {
    mOutputStreamDriver->RequestFrameCapture();
  }
}

nsresult CanvasCaptureMediaStream::Init(
    const dom::Optional<double>& aFPS, nsIPrincipal* aPrincipal,
    CanvasCaptureTrackSource* aTrackSource) {
  MediaTrackGraph* graph = MediaTrackGraph::GetInstance(
      MediaTrackGraph::SYSTEM_THREAD_DRIVER, GetOwnerWindow(),
      MediaTrackGraph::REQUEST_DEFAULT_SAMPLE_RATE,
      MediaTrackGraph::DEFAULT_OUTPUT_DEVICE);
  SourceMediaTrack* source = graph->CreateSourceTrack(MediaSegment::VIDEO);
  PrincipalHandle principalHandle = MakePrincipalHandle(aPrincipal);
  if (!aFPS.WasPassed()) {
    mOutputStreamDriver = new AutoDriver(source, principalHandle, aTrackSource);
  } else if (aFPS.Value() < 0) {
    return NS_ERROR_ILLEGAL_VALUE;
  } else {
    // Cap frame rate to 60 FPS for sanity
    double fps = std::min(60.0, aFPS.Value());
    mOutputStreamDriver =
        new TimerDriver(source, fps, principalHandle, aTrackSource);
  }
  return NS_OK;
}

FrameCaptureListener* CanvasCaptureMediaStream::FrameCaptureListener() {
  return mOutputStreamDriver;
}

void CanvasCaptureMediaStream::StopCapture() {
  if (!mOutputStreamDriver) {
    return;
  }

  mOutputStreamDriver->EndTrack();
  mOutputStreamDriver = nullptr;
}

SourceMediaTrack* CanvasCaptureMediaStream::GetSourceStream() const {
  if (!mOutputStreamDriver) {
    return nullptr;
  }
  return mOutputStreamDriver->mSourceStream;
}

}  // namespace mozilla::dom
