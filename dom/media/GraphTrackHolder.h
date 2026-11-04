/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_GRAPHTRACKHOLDER_H_
#define DOM_MEDIA_GRAPHTRACKHOLDER_H_

#include "MediaStreamTrack.h"
#include "MediaTrackConstraints.h"
#include "mozilla/MozPromise.h"
#include "mozilla/RefPtr.h"
#include "mozilla/StateMirroring.h"
#include "mozilla/StateWatching.h"
#include "nsThreadUtils.h"

namespace mozilla {

class MediaInputPort;
class MediaTrack;
class MediaTrackGraph;
class ProcessedMediaTrack;

namespace dom {

/**
 * Owns the MediaTrackGraph track of a MediaStreamTrack, which borrows it. Only
 * the main thread creates and destroys graph objects, so this lets tracks on
 * other threads, i.e., in dedicated workers, have a graph track too. It also
 * lets a MediaStreamTrack stay tied to its source as if it was created in the
 * source's context when it is transferred, per the MediaStreamTrack transfer
 * steps in
 * https://w3c.github.io/mediacapture-extensions/#transferable-mediastreamtrack
 *
 * There is one holder, and one graph track, per MediaStreamTrack, which moves
 * with the track when it is transferred:
 *  - A MediaStreamTrack on the main thread owns its holder for as long as it
 *    lives, also once it has ended, when the holder has shut down. The holder
 *    is unattached: the track's own sink is the source's sink and the holder
 *    does not reference the source. The track owning it would otherwise reach
 *    the source through a reference the cycle collector cannot see.
 *  - When the track is transferred the holder moves to the transferred data
 *    and Attach()es to the source. Now it is a keep-alive sink of the source,
 *    which stays alive and follows the enabled state of the track on the other
 *    thread, and it publishes the state of the track through Canonicals for
 *    the track on the other thread to mirror.
 *  - A track transferred to a worker is owned by that worker's
 *    WorkerTrackSourceProxy, which keeps the holder attached. The track on the
 *    main thread that the holder is then transferred to takes it over as its
 *    own, and calls Unattach().
 *  - A clone of a track on a worker gets a holder that is pending until it is
 *    initialized on the main thread as a clone of the original's. See
 *    CreateClone().
 *
 * The holder is refcounted thread-safely, may be AddRefed and Released on any
 * thread and is destroyed on the main thread when the last reference is
 * released. Everything else is main thread only.
 *
 * The graph track is destroyed in Shutdown() and the destructor, not when the
 * holder ends because the source or the graph track ended, so the track
 * borrowing it can rely on it until its owner shuts the holder down. An owner
 * that ends calls Shutdown() but keeps its reference, so that it can still
 * pass the holder on, and its source, if it is transferred while ended.
 *
 * What the previous owner of a transferred holder queued for the graph track
 * from its own thread, like disconnecting mirrors, may not have reached the
 * MediaTrackGraph when the holder is shut down by the next owner, and the graph
 * track must not be destroyed before. A transferring track therefore gives the
 * holder a fence, see SetFence(), which Shutdown() waits for, and which keeps
 * the holder alive.
 *
 * TODO(Bug 1991618): The holder always lives on the main thread, as does the
 * source. A source owned by a worker (e.g., a VideoTrackGenerator in a worker
 * whose track is transferred to the main thread or another worker) would need
 * the holder to live on that worker, kept alive with a ThreadSafeWorkerRef
 * while other threads may dispatch to it.
 */
class GraphTrackHolder final : public MediaStreamTrackSource::Sink {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING_WITH_DELETE_ON_MAIN_THREAD(
      GraphTrackHolder)

  /**
   * What the holder provides to the track on another thread: the graph track
   * it borrows for as long as it holds the holder, and the Canonicals of the
   * holder's source. All null unless the holder is attached and live.
   */
  struct HolderState {
    RefPtr<ProcessedMediaTrack> mGraphTrack;
    RefPtr<AbstractCanonical<MediaStreamTrackSourceSettings>> mSettings;
    RefPtr<AbstractCanonical<MediaStreamTrackSourceCapabilities>> mCapabilities;
    bool operator==(const HolderState&) const = default;
  };

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
   * Creates a pending holder for a clone of aOriginal's track, for a track on
   * another thread. Any thread. aEnabled and aMuted are the clone's states,
   * which CanonicalMuted() reports until the holder is initialized on the main
   * thread, by cloning aOriginal's source where it supports that, and attached
   * to it. A clone of an ended holder gets a graph track without input in the
   * original's graph, which ends it at once, unless the original has no graph
   * track.
   */
  static already_AddRefed<GraphTrackHolder> CreateClone(
      GraphTrackHolder* aOriginal, bool aEnabled, bool aMuted);

  /**
   * Makes this holder aSource's sink, on behalf of the track on the other
   * thread that the holder is transferred to. aMuted is the muted state of the
   * track being transferred, which the source does not expose itself. Until
   * now the main-thread MediaStreamTrack owning the holder was the sink.
   * Takes a reference to aSource, which is the source of the transferred track
   * even if the holder has ended, until Unattach() or until destroyed.
   */
  void Attach(MediaStreamTrackSource* aSource, bool aMuted);

  /**
   * For a main-thread MediaStreamTrack taking the holder over from a transfer,
   * after it has registered its own sink with the source. Unregisters this
   * holder as sink if it is, and drops the reference to the source.
   */
  void Unattach();

  /**
   * Makes the holder wait for aFence before destroying the graph track, and
   * stay alive until then. The track that the holder is transferred from
   * ensures that aFence resolves after what it queued for the graph track. Any
   * thread. Replaces an earlier fence, as the transferring track's fence
   * resolves after that.
   */
  void SetFence(TransferFencePromise* aFence);

  /**
   * Ends the holder if live, and destroys the graph track, once the fence has
   * resolved if there is one. Idempotent. The owner keeps its reference.
   */
  void Shutdown();

  /**
   * Shuts the holder down on the main thread. For an owner on another thread.
   */
  void ShutdownAsync();

  // Whether we reference a source, i.e., are attached. A pending holder is
  // attached by the time it is transferred to the main thread.
  bool Attached() const {
    MOZ_ASSERT(NS_IsMainThread());
    return !!mSource;
  }
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
    return mEnded.Ref();
  }

  /**
   * Sets the enabled state of the track tied to this holder. Once attached,
   * applies to the graph track and the source like MediaStreamTrack::SetEnabled
   * does.
   */
  void SetEnabled(bool aEnabled);

  /**
   * Like SetEnabled for the track on another thread that the holder is
   * attached on behalf of, which is applied on the main thread. Ignored if the
   * holder is no longer attached by then.
   */
  void SetEnabledAsync(bool aEnabled);

  // The holder's state, owned by the main thread, for the track on another
  // thread to mirror. The accessors may be called from any thread.
  AbstractCanonical<bool>* CanonicalMuted() { return &mMuted; }
  AbstractCanonical<bool>* CanonicalEnded() { return &mEnded; }
  AbstractCanonical<HolderState>* CanonicalHolderState() {
    return &mHolderState;
  }

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
                   bool aEnabled, bool aMuted, bool aPending);
  ~GraphTrackHolder();

  // Creates the graph track of type aType in aGraph, forwarding aInputTrack if
  // given. Without input it ends at once.
  void InitGraphTrack(MediaSegment::Type aType, MediaTrackGraph* aGraph,
                      mozilla::MediaTrack* aInputTrack);

  // Initializes a pending holder as a clone of aOriginal. Main thread.
  void InitializeClone(GraphTrackHolder* aOriginal);

  // Called on the main thread when mTrackEnded changes.
  void OnTrackEnded();

  void OnFenceResolved(const TransferFencePromise::ResolveOrRejectValue&);

  // Unregisters from the source if attached, stops mirroring the graph track
  // and publishes that we ended. Leaves the graph track alive. Idempotent.
  void End();

  // Any thread.
  template <typename Function>
  static void DispatchToMainThread(const char* aName, Function&& aFunction);

  // Set by Attach() and cleared by Unattach(). Invisible to the cycle
  // collector, which therefore cannot unlink the source through us. We must
  // drop it once no track needs it.
  RefPtr<MediaStreamTrackSource> mSource;
  // The input MediaTrack assigned us by the data producer. Owned by the
  // producer, which decides its lifetime. Null if created ended or after
  // Shutdown().
  RefPtr<mozilla::MediaTrack> mInputTrack;
  // The MediaTrack representing the MediaStreamTrack in the MediaTrackGraph,
  // which it borrows. Set on construction if we have an input track. Owned by
  // us. Destroyed only in Shutdown().
  RefPtr<ProcessedMediaTrack> mTrack;
  // The MediaInputPort connecting mInputTrack to mTrack. Set on construction
  // if we have an input track. Owned by us. Destroyed only in Shutdown().
  RefPtr<MediaInputPort> mPort;
  // Pending until the previous owner's operations on mTrack have reached the
  // MediaTrackGraph, see SetFence(), and our request for it.
  RefPtr<TransferFencePromise> mFence;
  MozPromiseRequestHolder<TransferFencePromise> mFenceRequest;
  WatchManager<GraphTrackHolder> mWatchManager;
  // Mirrors mTrack's ended state from the MediaTrackGraph while live.
  Mirror<bool> mTrackEnded;
  Canonical<bool> mMuted;
  // Whether we have ended. See End().
  Canonical<bool> mEnded;
  Canonical<HolderState> mHolderState;
  bool mEnabled;
};

}  // namespace dom
}  // namespace mozilla

#endif  // DOM_MEDIA_GRAPHTRACKHOLDER_H_
