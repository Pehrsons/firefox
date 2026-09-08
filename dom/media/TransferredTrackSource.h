/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_TRANSFERREDTRACKSOURCE_H_
#define DOM_MEDIA_TRANSFERREDTRACKSOURCE_H_

#include "MediaStreamTrack.h"
#include "MediaStreamTrackSourceHandle.h"
#include "mozilla/StateMirroring.h"
#include "mozilla/StateWatching.h"
#include "nsCOMPtr.h"
#include "nsISerialEventTarget.h"

namespace mozilla::dom {

class ThreadSafeWorkerRef;

/**
 * The MediaStreamTrackSource of MediaStreamTracks that were transferred to a
 * thread other than the main thread, i.e., to a dedicated worker, and of their
 * clones. It lives on the receiving thread and proxies to the main-thread
 * GraphTrackHolder through a MediaStreamTrackSourceHandle, so that the original
 * context stays in control of the source per
 * https://w3c.github.io/mediacapture-extensions/#transferable-mediastreamtrack
 *
 * The holder's state arrives through Mirrors of the handle's Canonicals and is
 * forwarded to our track, registered as our sink, including the graph track it
 * borrows from the holder. The track mirrors that graph track's state from the
 * MediaTrackGraph itself. Settings and capabilities are mirrored from the
 * holder's source once the holder is known, and republished through this
 * source's own Canonicals. Requests from the track (enabled state, stopping,
 * constraints) go back through the handle. Dropping the handle when detaching
 * lets the holder go away and the original source stop.
 *
 * Cloning creates a new TransferredTrackSource with a pending handle, whose
 * holder is cloned on the main thread.
 *
 * Worker shutdown: on a worker thread this holds a StrongWorkerRef, through a
 * ThreadSafeWorkerRef, until it has detached from the handle. The WorkerRef
 * callback ends our track and detaches from a task on the worker, which the
 * ref keeps running for, after which nothing may touch the worker thread again
 * on this source's behalf.
 *
 * Asynchronous round trips to the main thread or the graph started from this
 * thread follow one of two patterns:
 * - Handlers whose result must land, like the outcome of applyConstraints(),
 *   capture mWorkerRef. The worker's run loop keeps going until the handler has
 *   run, and the worker will not have been destroyed underneath it.
 * - Handlers that may be dropped once we are detached track their request in a
 *   MozPromiseRequestHolder and are disconnected in Detach(), rather than
 *   relying on the dispatch failing.
 */
class TransferredTrackSource final : public MediaStreamTrackSource {
 public:
  NS_DECL_ISUPPORTS_INHERITED

  /**
   * Creates a source on the current thread for the transferred track described
   * by aData. Returns nullptr if the current worker is already shutting down.
   */
  static already_AddRefed<TransferredTrackSource> Create(
      const MediaStreamTrack::TransferredData& aData);

  // MediaStreamTrackSource
  already_AddRefed<MediaStreamTrackSourceHandle> TransferHandle() override;
  CloneResult Clone() override;
  void Destroy() override;
  MediaSourceEnum GetMediaSource() const override { return mMediaSource; }
  bool HasAlpha() const override { return mHasAlpha; }
  RefPtr<ApplyConstraintsPromise> ApplyConstraints(
      const MediaTrackConstraints& aConstraints,
      CallerType aCallerType) override;
  void Stop() override;
  void Disable() override;
  void Enable() override;

 private:
  using HolderState = MediaStreamTrackSourceHandle::HolderState;

  TransferredTrackSource(
      MediaStreamTrackSourceHandle* aHandle, AbstractThread* aOwnerThread,
      const nsAString& aLabel, MediaSourceEnum aMediaSource, bool aHasAlpha,
      bool aMuted, const MediaStreamTrackSourceSettings& aSettings,
      const MediaStreamTrackSourceCapabilities& aCapabilities,
      const nsAString& aDeviceId, const nsAString& aGroupId,
      ProcessedMediaTrack* aGraphTrack);
  ~TransferredTrackSource();

  // Takes a worker ref if on a worker and attaches to the handle. Returns
  // false if the worker is shutting down, in which case this is detached.
  bool Init(bool aEnabled);

  // Stops interacting with mHandle and the worker. Idempotent. Owner thread.
  // Must be called after our track has ended, see the implementation.
  void Detach();

  // Resolves once the holder's state is known and the settings mirrors have
  // connected, so that requests to the holder are ordered after them.
  // Rejected on Detach().
  RefPtr<GenericPromise> HolderReady();
  RefPtr<ApplyConstraintsPromise> ProxyApplyConstraints(
      const MediaTrackConstraints& aConstraints, CallerType aCallerType);

  // Watch callbacks. Owner thread.
  void OnMutedChanged();
  void OnEnded();
  void OnHolderStateChanged();
  void OnSettingsChanged();
  void OnCapabilitiesChanged();

  // Null once detached.
  RefPtr<MediaStreamTrackSourceHandle> mHandle;
  const nsCOMPtr<nsISerialEventTarget> mOwnerThread;
  const RefPtr<AbstractThread> mAbstractThread;
  const MediaSourceEnum mMediaSource;
  const bool mHasAlpha;
  WatchManager<TransferredTrackSource> mWatchManager;
  // Mirrors of mHandle's Canonicals, connected until Detach(). Their initial
  // values are the snapshots taken at transfer or cloning.
  Mirror<bool> mMuted;
  Mirror<bool> mEnded;
  // The graph track our track borrows, once known, and the holder's source's
  // Canonicals. The graph track is owned by the holder, which outlives our use
  // of mHandle.
  Mirror<HolderState> mHolderState;
  // Mirrors of the holder's source's Canonicals, connected from when
  // mHolderState has them until Detach().
  Mirror<MediaStreamTrackSourceSettings> mOriginalSettings;
  Mirror<MediaStreamTrackSourceCapabilities> mOriginalCapabilities;
  MozPromiseHolder<GenericPromise> mHolderReadyPromise;
  bool mHolderReady = false;
  // Set when on a worker thread. Null on the main thread and after Detach().
  // Copies are captured by handlers that must run on this thread, see the
  // class comment.
  RefPtr<ThreadSafeWorkerRef> mWorkerRef;
  bool mDetached = false;
};

}  // namespace mozilla::dom

#endif  // DOM_MEDIA_TRANSFERREDTRACKSOURCE_H_
