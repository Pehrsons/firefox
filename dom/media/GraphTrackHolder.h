/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_GRAPHTRACKHOLDER_H_
#define DOM_MEDIA_GRAPHTRACKHOLDER_H_

#include "MediaStreamTrack.h"
#include "mozilla/RefPtr.h"
#include "mozilla/StateMirroring.h"
#include "mozilla/StateWatching.h"
#include "mozilla/ThreadSafeWeakPtr.h"
#include "nsThreadUtils.h"

namespace mozilla {

class MediaInputPort;
class MediaTrack;
class MediaTrackGraph;
class ProcessedMediaTrack;

namespace dom {

class MediaStreamTrackSourceHandle;

/**
 * Owns the MediaTrackGraph track of a MediaStreamTrack, which borrows it. Only
 * the main thread creates and destroys graph objects, so this lets tracks on
 * other threads, i.e., in dedicated workers, have a graph track too.
 *
 * A main-thread MediaStreamTrack creates its own holder and is the source's
 * sink itself. When the track is transferred, the holder moves to a
 * MediaStreamTrackSourceHandle and takes on the main-thread duties of the
 * track on the other thread: it becomes a keep-alive sink of the source, so
 * the source stays alive and follows that track's enabled state, and notifies
 * the handle of state changes. See Attach().
 *
 * The graph track is destroyed in Shutdown() and nowhere else, not even when
 * the holder ends because the source or the graph track ended, so the track
 * borrowing it can rely on it until the holder's owner shuts the holder down.
 * The holder is refcounted since pending watch callbacks hold it, so the owner
 * must Shutdown() it before dropping its reference.
 *
 * Main thread only.
 */
class GraphTrackHolder final : public MediaStreamTrackSource::Sink {
 public:
  NS_INLINE_DECL_REFCOUNTING(GraphTrackHolder)

  /**
   * Creates a holder for aInputTrack, the MediaTrack that a MediaStreamTrack's
   * producer produces its media into, giving it a graph track in aGraph
   * forwarding aInputTrack. aGraph is aInputTrack's graph unless aInputTrack
   * has been destroyed. The holder starts out ended, without a graph track, if
   * aInputTrack is null. aEnabled is the initial state for Enabled().
   */
  static already_AddRefed<GraphTrackHolder> Create(
      mozilla::MediaTrack* aInputTrack, MediaTrackGraph* aGraph, bool aEnabled);

  /**
   * Makes this holder aSource's sink on behalf of the track aHandle is for,
   * and notify aHandle of state changes for as long as it is alive. Until then
   * the main-thread MediaStreamTrack that created the holder is the sink, and
   * the holder does not reference the source: the track owning it would
   * otherwise reach the source through a reference the cycle collector cannot
   * see.
   */
  void Attach(MediaStreamTrackSourceHandle* aHandle,
              MediaStreamTrackSource* aSource);

  /**
   * Ends the holder if live, and destroys the graph track.
   */
  void Shutdown();

  // Once attached.
  MediaStreamTrackSource& Source() const {
    MOZ_ASSERT(NS_IsMainThread());
    MOZ_ASSERT(mSource);
    return *mSource;
  }
  mozilla::MediaTrack* InputTrack() const {
    MOZ_ASSERT(NS_IsMainThread());
    return mInputTrack;
  }
  // The graph track, borrowed by the MediaStreamTrack. Null if created ended
  // or after Shutdown().
  ProcessedMediaTrack* GraphTrack() const {
    MOZ_ASSERT(NS_IsMainThread());
    return mTrack;
  }
  bool Ended() const {
    MOZ_ASSERT(NS_IsMainThread());
    return mEnded;
  }

  /**
   * Sets the enabled state of the track on the other thread. Applies to the
   * source like MediaStreamTrack::SetEnabled does once attached.
   */
  void SetEnabled(bool aEnabled);

  // MediaStreamTrackSource::Sink
  bool KeepsSourceAlive() const override { return true; }
  bool Enabled() const override {
    MOZ_ASSERT(NS_IsMainThread());
    return mEnabled;
  }
  // Not forwarded. The track on the other thread has no principal. Consumers
  // there check the PrincipalHandle of the data in the MediaTrackGraph.
  void PrincipalChanged() override {}
  void MutedChanged(bool aNewState) override;
  void ConstraintsChanged(const MediaTrackConstraints& aConstraints) override {}
  void OverrideEnded() override;

 private:
  GraphTrackHolder(mozilla::MediaTrack* aInputTrack, MediaTrackGraph* aGraph,
                   bool aEnabled);
  ~GraphTrackHolder();

  // Called on the main thread when mTrackEnded changes.
  void OnTrackEnded();

  // Unregisters from the source if attached, stops mirroring the graph track
  // and notifies the handle. Leaves the graph track alive. Idempotent.
  void End();

  // The handle owning us once attached, notified of state changes while alive.
  ThreadSafeWeakPtr<MediaStreamTrackSourceHandle> mHandle;
  // Set by Attach(). Invisible to the cycle collector, which therefore cannot
  // unlink the source through us. We must drop it once no track needs it.
  RefPtr<MediaStreamTrackSource> mSource;
  // The input MediaTrack assigned us by the data producer. Owned by the
  // producer, which decides its lifetime. Null if created ended.
  const RefPtr<mozilla::MediaTrack> mInputTrack;
  // The MediaTrack representing the MediaStreamTrack in the MediaTrackGraph,
  // which it borrows. Set on construction if we have an input track. Owned by
  // us. Destroyed only in Shutdown().
  RefPtr<ProcessedMediaTrack> mTrack;
  // The MediaInputPort connecting mInputTrack to mTrack. Set on construction
  // if we have an input track. Owned by us. Destroyed only in Shutdown().
  RefPtr<MediaInputPort> mPort;
  WatchManager<GraphTrackHolder> mWatchManager;
  // Mirrors mTrack's ended state from the MediaTrackGraph while live.
  Mirror<bool> mTrackEnded;
  bool mEnabled;
  bool mAttached = false;
  bool mEnded = false;
};

}  // namespace dom
}  // namespace mozilla

#endif  // DOM_MEDIA_GRAPHTRACKHOLDER_H_
