/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_GRAPHTRACKHOLDER_H_
#define DOM_MEDIA_GRAPHTRACKHOLDER_H_

#include "mozilla/RefPtr.h"
#include "nsISupportsImpl.h"
#include "nsThreadUtils.h"

namespace mozilla {

class MediaInputPort;
class MediaTrack;
class MediaTrackGraph;
class ProcessedMediaTrack;

namespace dom {

/**
 * Owns the MediaTrackGraph track of a MediaStreamTrack, which borrows it.
 *
 * The graph track is destroyed in Shutdown() and nowhere else, so the track
 * borrowing it can rely on it until the holder's owner shuts the holder down.
 * The owner must Shutdown() the holder before dropping its reference.
 *
 * Main thread only.
 */
class GraphTrackHolder final {
 public:
  NS_INLINE_DECL_REFCOUNTING(GraphTrackHolder)

  /**
   * Creates a holder for aInputTrack, the MediaTrack that a MediaStreamTrack's
   * producer produces its media into, giving it a graph track in aGraph
   * forwarding aInputTrack. aGraph is aInputTrack's graph unless aInputTrack
   * has been destroyed.
   */
  static already_AddRefed<GraphTrackHolder> Create(
      mozilla::MediaTrack* aInputTrack, MediaTrackGraph* aGraph);

  /**
   * Destroys the graph track.
   */
  void Shutdown();

  mozilla::MediaTrack* InputTrack() const {
    MOZ_ASSERT(NS_IsMainThread());
    return mInputTrack;
  }
  // The graph track, borrowed by the MediaStreamTrack. Null after Shutdown().
  ProcessedMediaTrack* GraphTrack() const {
    MOZ_ASSERT(NS_IsMainThread());
    return mTrack;
  }

 private:
  GraphTrackHolder(mozilla::MediaTrack* aInputTrack, MediaTrackGraph* aGraph);
  ~GraphTrackHolder();

  // The input MediaTrack assigned us by the data producer. Owned by the
  // producer, which decides its lifetime.
  const RefPtr<mozilla::MediaTrack> mInputTrack;
  // The MediaTrack representing the MediaStreamTrack in the MediaTrackGraph,
  // which it borrows. Set on construction. Owned by us. Destroyed only in
  // Shutdown().
  RefPtr<ProcessedMediaTrack> mTrack;
  // The MediaInputPort connecting mInputTrack to mTrack. Set on construction.
  // Owned by us. Destroyed only in Shutdown().
  RefPtr<MediaInputPort> mPort;
};

}  // namespace dom
}  // namespace mozilla

#endif  // DOM_MEDIA_GRAPHTRACKHOLDER_H_
