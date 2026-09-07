/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_MEDIASTREAMTRACKSOURCEHANDLE_H_
#define DOM_MEDIA_MEDIASTREAMTRACKSOURCEHANDLE_H_

#include "GraphTrackHolder.h"
#include "MediaTrackConstraints.h"
#include "mozilla/RefPtr.h"
#include "mozilla/StateMirroring.h"
#include "mozilla/ThreadSafeWeakPtr.h"

namespace mozilla {

class MediaTrack;

namespace dom {

class MediaStreamTrackSource;

/**
 * A thread-safe handle owning a main-thread GraphTrackHolder, through which
 * MediaStreamTracks on other threads (in dedicated workers) stay tied to their
 * source as if they were created in the source's context, per the
 * MediaStreamTrack transfer steps in
 * https://w3c.github.io/mediacapture-extensions/#transferable-mediastreamtrack
 *
 * The handle is dataHolder.[[source]] in those steps. The holder keeps the
 * source alive, applies the enabled state of the track on the other thread to
 * it, and owns the graph track of the transferred track. The holder, and thus
 * that graph track, lives for as long as the handle does. The handle is held
 * by a MediaStreamTrack::TransferredData in flight and then by the
 * TransferredTrackSource on the receiving thread. When the last reference goes
 * away the holder is destroyed, which lets the source stop.
 *
 * The holder's state is published through Canonicals owned by the main
 * thread, for the receiving thread to mirror. Settings and capabilities are
 * the Canonicals of the holder's source, handed over through HolderState.
 *
 * TODO(Bug 1991618): The holder always lives on the main thread, as does the
 * source. A source owned by a worker (e.g., a VideoTrackGenerator in a worker
 * whose track is transferred to the main thread or another worker) would need
 * the holder to live on that worker, kept alive with a ThreadSafeWorkerRef
 * while other threads may dispatch to it.
 */
class MediaStreamTrackSourceHandle final
    : public SupportsThreadSafeWeakPtr<MediaStreamTrackSourceHandle> {
 public:
  MOZ_DECLARE_REFCOUNTED_TYPENAME(MediaStreamTrackSourceHandle)

  /**
   * What the holder provides to the track on the receiving thread: the
   * Canonicals of the holder's source. Null if the holder was created ended.
   */
  struct HolderState {
    RefPtr<AbstractCanonical<MediaStreamTrackSourceSettings>> mSettings;
    RefPtr<AbstractCanonical<MediaStreamTrackSourceCapabilities>> mCapabilities;
    bool operator==(const HolderState&) const = default;
  };

  /**
   * Creates a handle owning aHolder, the transferred track's GraphTrackHolder,
   * and attaches it to aSource, the transferred track's source. Main thread
   * only. aEnabled and aMuted are the transferred track's enabled and muted
   * states, which the source does not expose itself.
   */
  static already_AddRefed<MediaStreamTrackSourceHandle> Create(
      already_AddRefed<GraphTrackHolder> aHolder,
      MediaStreamTrackSource* aSource, bool aEnabled, bool aMuted);

  // Main thread only.
  GraphTrackHolder* Holder() const;

  /**
   * Sets whether the track tied to this handle wants the source enabled.
   * Called on the receiving thread, applied on the main thread.
   */
  void SetEnabled(bool aEnabled);

  // The holder's state, owned by the main thread, for the receiving thread to
  // mirror. The accessors may be called from any thread.
  AbstractCanonical<bool>* CanonicalMuted() { return &mMuted; }
  AbstractCanonical<bool>* CanonicalEnded() { return &mEnded; }
  AbstractCanonical<HolderState>* CanonicalHolderState() {
    return &mHolderState;
  }

  // Called by the holder. Main thread only.
  void HolderMutedChanged(bool aMuted);
  void HolderEnded();

  ~MediaStreamTrackSourceHandle();

 private:
  MediaStreamTrackSourceHandle(bool aEnabled, bool aMuted);

  // Main thread only.
  void SetHolder(already_AddRefed<GraphTrackHolder> aHolder,
                 MediaStreamTrackSource* aSource);
  void ApplyEnabled(bool aEnabled);

  // Any thread.
  template <typename Function>
  static void DispatchToMainThread(const char* aName, Function&& aFunction);

  // Main thread only. Shut down and released on the main thread.
  RefPtr<GraphTrackHolder> mHolder;
  // Main thread only.
  Canonical<bool> mMuted;
  Canonical<bool> mEnded;
  Canonical<HolderState> mHolderState;
  // Main thread only. The last state passed to SetEnabled().
  bool mEnabled;
};

}  // namespace dom
}  // namespace mozilla

#endif  // DOM_MEDIA_MEDIASTREAMTRACKSOURCEHANDLE_H_
