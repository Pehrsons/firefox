/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_TRANSFERREDTRACKSOURCE_H_
#define DOM_MEDIA_TRANSFERREDTRACKSOURCE_H_

#include "MediaStreamTrack.h"
#include "MediaStreamTrackSourceHandle.h"
#include "PrincipalHandle.h"
#include "nsCOMPtr.h"
#include "nsISerialEventTarget.h"

namespace mozilla::dom {

class StrongWorkerRef;

/**
 * The MediaStreamTrackSource of MediaStreamTracks that were transferred to a
 * thread other than the one owning the original source, i.e., to a dedicated
 * worker. It lives on the receiving thread and proxies to the original source
 * through a MediaStreamTrackSourceHandle, so that the original context stays
 * in control of the source per
 * https://w3c.github.io/mediacapture-extensions/#transferable-mediastreamtrack
 *
 * Notifications from the original source arrive through the handle and are
 * forwarded to the tracks registered as sinks. Requests from those tracks
 * (enabled state, stopping) go back through the handle.
 *
 * Worker shutdown: on a worker thread this holds a StrongWorkerRef until it
 * has detached from the handle. The WorkerRef callback detaches, after which
 * nothing may touch the worker thread again on this source's behalf. Runnables
 * the handle has already dispatched only hold thread-safe objects and become
 * no-ops once detached.
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
  already_AddRefed<MediaStreamTrackSourceHandle> CreateTransferHandle(
      mozilla::MediaTrack* aInputTrack) override;
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

  explicit TransferredTrackSource(
      const MediaStreamTrack::TransferredData& aData);
  ~TransferredTrackSource();

  // Stops interacting with mHandle and the worker. Idempotent. Owner thread.
  void Detach();

  // Called through mReceiver on the owner thread.
  void OnPrincipalChanged(const PrincipalHandle& aPrincipalHandle);
  void OnMutedChanged(bool aNewState);
  void OnConstraintsChanged(const MediaTrackConstraints& aConstraints);
  void OnOverrideEnded();

  const RefPtr<MediaStreamTrackSourceHandle> mHandle;
  const RefPtr<Receiver> mReceiver;
  const nsCOMPtr<nsISerialEventTarget> mOwnerThread;
  const MediaSourceEnum mMediaSource;
  const bool mHasAlpha;
  // TODO(Bug 1991619): Settings and capabilities are snapshots from the time
  // of transfer. The source should notify sinks when they change.
  MediaTrackSettings mSettings;
  MediaTrackCapabilities mCapabilities;
  // TODO(Bug 1991619): Principals are main-thread objects so
  // MediaStreamTrackSource::mPrincipal is null here. Sinks needing the
  // principal (i.e., the MediaTrackGraph integration) should use this handle.
  PrincipalHandle mPrincipalHandle;
  // Set when on a worker thread. Null on the main thread and after Detach().
  RefPtr<StrongWorkerRef> mWorkerRef;
  bool mDetached = false;
};

}  // namespace mozilla::dom

#endif  // DOM_MEDIA_TRANSFERREDTRACKSOURCE_H_
