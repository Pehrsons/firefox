/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_GRAPHTRACKHOLDER_H_
#define DOM_MEDIA_GRAPHTRACKHOLDER_H_

#include "MediaStreamTrack.h"
#include "mozilla/RefPtr.h"
#include "mozilla/StateMirroring.h"
#include "mozilla/StateWatching.h"

namespace mozilla {

class MediaInputPort;
class MediaTrack;
class ProcessedMediaTrack;

namespace dom {

/**
 * Holds the main-thread state of MediaStreamTracks living on other threads,
 * i.e., in dedicated workers. It takes on the main-thread duties of such a
 * track: it is a keep-alive sink of the source, so the source stays alive and
 * follows the enabled state of the tracks it holds for, and it owns the
 * MediaTrackGraph track that they borrow.
 *
 * Only the main thread creates and destroys graph objects. The holder's graph
 * track is destroyed in the destructor and nowhere else, not even when the
 * holder ends because the source or the graph track ended, so the tracks it
 * holds for can rely on it for as long as the holder exists.
 * MediaStreamTrackSourceHandle owns the holder and outlives all tracks
 * depending on it. The holder is refcounted since pending watch callbacks hold
 * it, so the owner must Shutdown() it before dropping its reference.
 *
 * Main thread only.
 */
class GraphTrackHolder final : public MediaStreamTrackSource::Sink {
 public:
  NS_INLINE_DECL_REFCOUNTING(GraphTrackHolder)

  /**
   * Notified by the holder. Implemented by MediaStreamTrackSourceHandle.
   */
  class Owner {
   public:
    virtual void HolderMutedChanged(bool aMuted) = 0;
    virtual void HolderConstraintsChanged(
        const MediaTrackConstraints& aConstraints,
        const MediaTrackSettings& aSettings) = 0;
    virtual void HolderEnded() = 0;

   protected:
    virtual ~Owner() = default;
  };

  /**
   * Creates a holder for aSource and aInputTrack, the MediaTrack aSource
   * feeds it with, giving it a graph track forwarding aInputTrack. The holder
   * starts out ended, without a graph track, if aInputTrack is null or already
   * destroyed. aEnabled is the initial state for Enabled().
   */
  static already_AddRefed<GraphTrackHolder> Create(
      Owner* aOwner, MediaStreamTrackSource* aSource,
      mozilla::MediaTrack* aInputTrack, bool aEnabled);

  /**
   * Creates a holder for a clone of this holder's source, with an independent
   * source where the source supports that. An ended holder clones into an
   * ended holder.
   */
  already_AddRefed<GraphTrackHolder> Clone(Owner* aOwner, bool aEnabled) const;

  /**
   * Ends the holder if live and forgets the owner, so that the owner can drop
   * its reference. The graph track stays alive until the destructor.
   */
  void Shutdown();

  MediaStreamTrackSource& Source() const { return *mSource; }
  mozilla::MediaTrack* InputTrack() const { return mInputTrack; }
  // The graph track borrowed by the tracks held for. Null if created ended.
  ProcessedMediaTrack* GraphTrack() const { return mTrack; }
  bool Ended() const { return mEnded; }

  /**
   * Sets the aggregate enabled state of the tracks held for. Applies to the
   * graph track and the source like MediaStreamTrack::SetEnabled does.
   */
  void SetEnabled(bool aEnabled);

  // MediaStreamTrackSource::Sink
  bool KeepsSourceAlive() const override { return true; }
  bool Enabled() const override { return mEnabled; }
  // The tracks held for mirror the PrincipalHandle of the graph track's data
  // from the MediaTrackGraph themselves.
  void PrincipalChanged() override {}
  void MutedChanged(bool aNewState) override;
  void ConstraintsChanged(const MediaTrackConstraints& aConstraints) override;
  void OverrideEnded() override;

 private:
  GraphTrackHolder(Owner* aOwner, MediaStreamTrackSource* aSource,
                   mozilla::MediaTrack* aInputTrack, bool aEnabled);
  ~GraphTrackHolder();

  // Called on the main thread when mTrackEnded changes.
  void OnTrackEnded();

  // Unregisters from the source and stops mirroring the graph track, and
  // notifies the owner. Leaves the graph track alive. Idempotent.
  void End();

  // Raw pointer is safe because the owner calls Shutdown(), which clears it,
  // before dropping its reference to us.
  Owner* mOwner;
  const RefPtr<MediaStreamTrackSource> mSource;
  // Owned by the producer, see MediaStreamTrack::mInputTrack.
  const RefPtr<mozilla::MediaTrack> mInputTrack;
  // Owned by us. Destroyed only in the destructor.
  RefPtr<ProcessedMediaTrack> mTrack;
  RefPtr<MediaInputPort> mPort;
  WatchManager<GraphTrackHolder> mWatchManager;
  // Mirrors mTrack's ended state from the MediaTrackGraph while live.
  Mirror<bool> mTrackEnded;
  bool mEnabled;
  bool mEnded = false;
};

}  // namespace dom
}  // namespace mozilla

#endif  // DOM_MEDIA_GRAPHTRACKHOLDER_H_
