/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CanvasCaptureMediaStream_h_
#define mozilla_dom_CanvasCaptureMediaStream_h_

#include "DOMMediaStream.h"
#include "MediaStreamTrack.h"
#include "PrincipalHandle.h"
#include "mozilla/WeakPtr.h"
#include "mozilla/dom/HTMLCanvasElement.h"

class nsIPrincipal;

namespace mozilla {
class DOMMediaStream;
class SourceMediaTrack;

namespace layers {
class Image;
}  // namespace layers

namespace dom {
class CanvasCaptureMediaStream;
class HTMLCanvasElement;
class OutputStreamFrameListener;

/*
 * The CanvasCaptureMediaStream is a MediaStream subclass that provides a video
 * track containing frames from a canvas. See an architectural overview below.
 *
 * ----------------------------------------------------------------------------
 *     === Main Thread ===              __________________________
 *                                     |                          |
 *                                     | CanvasCaptureMediaStream |
 *                                     |__________________________|
 *                                                  |
 *                                                  | RequestFrame()
 *                                                  v
 *                                       ________________________
 *  ________   FrameCaptureRequested?   |                        |
 * |        | ------------------------> |   OutputStreamDriver   |
 * | Canvas |  SetFrameCapture()        | (FrameCaptureListener) |
 * |________| ------------------------> |________________________|
 *                                                  |
 *                                                  | SetImage() -
 *                                                  | AppendToTrack()
 *                                                  |
 *                                                  v
 *                                      __________________________
 *                                     |                          |
 *                                     |  MTG / SourceMediaTrack  |
 *                                     |__________________________|
 * ----------------------------------------------------------------------------
 */

class CanvasCaptureTrackSource final : public MediaStreamTrackSource,
                                       public SupportsWeakPtr {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(CanvasCaptureTrackSource,
                                           MediaStreamTrackSource)

  CanvasCaptureTrackSource(nsIPrincipal* aPrincipal,
                           CanvasCaptureMediaStream* aCaptureStream);

  MediaSourceEnum GetMediaSource() const override {
    return MediaSourceEnum::Other;
  }
  bool HasAlpha() const override;
  void Stop() override;
  void Disable() override {}
  void Enable() override {}

  void SetCanvasSize(const CSSIntSize& aSize);

 private:
  virtual ~CanvasCaptureTrackSource() = default;

  RefPtr<CanvasCaptureMediaStream> mCaptureStream;
};

/*
 * Base class for drivers of the output stream.
 * It is up to each sub class to implement the NewFrame() callback of
 * FrameCaptureListener.
 */
class OutputStreamDriver : public FrameCaptureListener {
 public:
  OutputStreamDriver(SourceMediaTrack* aSourceStream,
                     const PrincipalHandle& aPrincipalHandle,
                     CanvasCaptureTrackSource* aTrackSource);

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(OutputStreamDriver);

  /*
   * Called from js' requestFrame() when it wants the next painted frame to be
   * explicitly captured.
   */
  virtual void RequestFrameCapture() = 0;

  /*
   * Sub classes can SetImage() to update the image being appended to the
   * output stream. It will be appended on the next NotifyPull from MTG.
   */
  void SetImage(RefPtr<layers::Image>&& aImage, const TimeStamp& aTime);

  /*
   * Ends the track in mSourceStream when we know there won't be any more images
   * requested for it.
   */
  void EndTrack();

  void CanvasSizeChanged(const CSSIntSize& aSize) override;

  const RefPtr<SourceMediaTrack> mSourceStream;
  const PrincipalHandle mPrincipalHandle;
  const WeakPtr<CanvasCaptureTrackSource> mTrackSource;

 protected:
  virtual ~OutputStreamDriver();
};

class CanvasCaptureMediaStream : public DOMMediaStream {
 public:
  CanvasCaptureMediaStream(nsIGlobalObject* aGlobal,
                           HTMLCanvasElement* aCanvas);

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(CanvasCaptureMediaStream,
                                           DOMMediaStream)

  nsresult Init(const dom::Optional<double>& aFPS, nsIPrincipal* aPrincipal,
                CanvasCaptureTrackSource* aTrackSource);

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  // WebIDL
  HTMLCanvasElement* Canvas() const { return mCanvas; }
  void RequestFrame();

  dom::FrameCaptureListener* FrameCaptureListener();

  /**
   * Stops capturing for this stream at mCanvas.
   */
  void StopCapture();

  SourceMediaTrack* GetSourceStream() const;

 protected:
  ~CanvasCaptureMediaStream();

 private:
  RefPtr<HTMLCanvasElement> mCanvas;
  RefPtr<OutputStreamDriver> mOutputStreamDriver;
};

}  // namespace dom
}  // namespace mozilla

#endif /* mozilla_dom_CanvasCaptureMediaStream_h_ */
