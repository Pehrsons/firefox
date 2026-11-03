/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef VIDEOSTREAMTRACK_H_
#define VIDEOSTREAMTRACK_H_

#include "DOMMediaStream.h"
#include "MediaStreamTrack.h"

namespace mozilla {

class VideoFrameContainer;
class VideoOutput;

namespace dom {

class VideoStreamTrack : public MediaStreamTrack {
 public:
  VideoStreamTrack(
      nsIGlobalObject* aGlobal, mozilla::MediaTrack* aInputTrack,
      MediaStreamTrackSource* aSource,
      MediaStreamTrackState aState = MediaStreamTrackState::Live,
      bool aMuted = false,
      const MediaTrackConstraints& aConstraints = MediaTrackConstraints(),
      MediaTrackGraph* aFallbackGraph = nullptr);

  VideoStreamTrack(nsIGlobalObject* aGlobal,
                   already_AddRefed<GraphTrackHolder> aHolder,
                   MediaStreamTrackSource* aSource,
                   MediaStreamTrackState aReadyState, bool aMuted,
                   const MediaTrackConstraints& aConstraints);

  already_AddRefed<MediaStreamTrack> Clone(nsIGlobalObject* aGlobal) override;

  void Destroy() override;

  VideoStreamTrack* AsVideoStreamTrack() override { return this; }
  const VideoStreamTrack* AsVideoStreamTrack() const override { return this; }

  void AddVideoOutput(VideoOutput* aOutput);
  void RemoveVideoOutput(VideoOutput* aOutput);

  /**
   * Whether this VideoStreamTrack's video frames will have an alpha channel.
   */
  bool HasAlpha() const { return GetSource().HasAlpha(); }

  // WebIDL
  void GetKind(nsAString& aKind) override { aKind.AssignLiteral("video"); }

  void GetLabel(nsAString& aLabel, CallerType aCallerType) override;

 private:
  nsTArray<RefPtr<VideoOutput>> mVideoOutputs;
};

}  // namespace dom
}  // namespace mozilla

#endif /* VIDEOSTREAMTRACK_H_ */
