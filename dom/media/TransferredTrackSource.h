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

class StrongWorkerRef;

/**
 * The MediaStreamTrackSource of MediaStreamTracks that were transferred to a
 * thread other than the main thread, i.e., to a dedicated worker. It lives on
 * the receiving thread and proxies to the main-thread GraphTrackHolder through
 * a MediaStreamTrackSourceHandle, so that the original context stays in
 * control of the source per
 * https://w3c.github.io/mediacapture-extensions/#transferable-mediastreamtrack
 *
 * The holder's state arrives through Mirrors of the handle's Canonicals and is
 * forwarded to our track, registered as our sink. Settings and capabilities
 * are mirrored from the holder's source once the holder is known, and
 * republished through this source's own Canonicals. Requests from the track
 * (enabled state, stopping) go back through the handle. Dropping the handle
 * when detaching lets the holder go away and the original source stop.
 *
 * Worker shutdown: on a worker thread this holds a StrongWorkerRef until it
 * has detached from the handle. The WorkerRef callback detaches, after which
 * nothing may touch the worker thread again on this source's behalf.
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
      const nsAString& aDeviceId, const nsAString& aGroupId);
  ~TransferredTrackSource();

  // Takes a worker ref if on a worker and attaches to the handle. Returns
  // false if the worker is shutting down, in which case this is detached.
  bool Init(bool aEnabled);

  // Stops interacting with mHandle and the worker. Idempotent. Owner thread.
  void Detach();

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
  // values are the snapshots taken at transfer.
  Mirror<bool> mMuted;
  Mirror<bool> mEnded;
  Mirror<HolderState> mHolderState;
  // Mirrors of the holder's source's Canonicals, connected from when
  // mHolderState has them until Detach().
  Mirror<MediaStreamTrackSourceSettings> mOriginalSettings;
  Mirror<MediaStreamTrackSourceCapabilities> mOriginalCapabilities;
  bool mHolderReady = false;
  // Set when on a worker thread. Null on the main thread and after Detach().
  RefPtr<StrongWorkerRef> mWorkerRef;
  bool mDetached = false;
};

}  // namespace mozilla::dom

#endif  // DOM_MEDIA_TRANSFERREDTRACKSOURCE_H_
