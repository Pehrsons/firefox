/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_TRANSFERREDTRACKSOURCE_H_
#define DOM_MEDIA_TRANSFERREDTRACKSOURCE_H_

#include "MediaStreamTrack.h"
#include "MediaStreamTrackSourceHandle.h"
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
 * Notifications from the holder arrive through the handle and are forwarded to
 * the tracks registered as sinks, including the graph track they borrow from
 * the holder. The tracks mirror that graph track's state from the
 * MediaTrackGraph themselves. Requests from those tracks (enabled state,
 * stopping, constraints) go back through the handle.
 *
 * Cloning creates a new TransferredTrackSource with a pending handle, whose
 * holder is cloned on the main thread.
 *
 * Worker shutdown: on a worker thread this holds a StrongWorkerRef, through a
 * ThreadSafeWorkerRef, until it has detached from the handle. The WorkerRef
 * callback ends the tracks and detaches from a task on the worker, which the
 * ref keeps running for, after which nothing may touch the worker thread again
 * on this source's behalf. Runnables the handle has already dispatched only
 * hold thread-safe objects and become no-ops once detached.
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
  void GetSettings(MediaTrackSettings& aResult) override;
  void GetCapabilities(MediaTrackCapabilities& aResult) override;
  void Stop() override;
  void Disable() override;
  void Enable() override;

 private:
  class Receiver;

  TransferredTrackSource(MediaStreamTrackSourceHandle* aHandle,
                         const nsAString& aLabel, MediaSourceEnum aMediaSource,
                         bool aHasAlpha, const MediaTrackSettings& aSettings,
                         const MediaTrackCapabilities& aCapabilities);
  ~TransferredTrackSource();

  // Takes a worker ref if on a worker and attaches to the handle. Returns
  // false if the worker is shutting down, in which case this is detached.
  bool Init(bool aEnabled);

  // Stops interacting with mHandle and the worker. Idempotent. Owner thread.
  // Must be called after our tracks have ended, see the implementation.
  void Detach();

  // Called through mReceiver on the owner thread.
  void OnMutedChanged(bool aNewState);
  void OnConstraintsChanged(const MediaTrackConstraints& aConstraints,
                            const MediaTrackSettings& aSettings);
  void OnOverrideEnded();
  void OnGraphTrackAvailable(ProcessedMediaTrack* aTrack);

  const RefPtr<MediaStreamTrackSourceHandle> mHandle;
  const RefPtr<Receiver> mReceiver;
  const nsCOMPtr<nsISerialEventTarget> mOwnerThread;
  const MediaSourceEnum mMediaSource;
  const bool mHasAlpha;
  // Snapshots from the time of transfer or the latest constraints change,
  // since the source cannot be queried synchronously from this thread.
  MediaTrackSettings mSettings;
  MediaTrackCapabilities mCapabilities;
  // The graph track our tracks borrow, once known. Owned by the holder, which
  // outlives our registration with mHandle. Null once detached.
  RefPtr<ProcessedMediaTrack> mGraphTrack;
  // Set when on a worker thread. Null on the main thread and after Detach().
  // Copies are captured by handlers that must run on this thread, see the
  // class comment.
  RefPtr<ThreadSafeWorkerRef> mWorkerRef;
  // The enabled state last requested by our sinks, as told to mHandle.
  bool mEnabled;
  bool mDetached = false;
};

}  // namespace mozilla::dom

#endif  // DOM_MEDIA_TRANSFERREDTRACKSOURCE_H_
