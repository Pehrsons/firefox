/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_MEDIASTREAMTRACKSOURCEHANDLE_H_
#define DOM_MEDIA_MEDIASTREAMTRACKSOURCEHANDLE_H_

#include "GraphTrackHolder.h"
#include "MediaTrackConstraints.h"
#include "mozilla/Mutex.h"
#include "mozilla/RefPtr.h"
#include "mozilla/dom/MediaTrackSettingsBinding.h"
#include "nsCOMPtr.h"
#include "nsISerialEventTarget.h"
#include "nsISupportsImpl.h"
#include "nsTArray.h"

namespace mozilla {

class MediaTrack;
class ProcessedMediaTrack;

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
 * source alive, applies the aggregate enabled state of the handle's users to
 * it, and owns the MediaTrackGraph track the tracks on other threads borrow.
 * The handle guarantees the holder, and thus that graph track, outlives all
 * its users. A user is either a MediaStreamTrack::TransferredData in flight,
 * or a TransferredTrackSource on the receiving thread. When the last user goes
 * away the holder is destroyed, which lets the source stop.
 *
 * A handle created with CreateClone() starts out pending: its holder is
 * created on the main thread by cloning the original handle's holder, which
 * gives it an independent source where the source supports that. Users and
 * listeners can be added while pending.
 *
 * Notifications from the holder are forwarded to all registered Listeners by
 * dispatching to their respective threads. Listeners must be thread-safe
 * refcounted since the dispatched runnables hold them, and may be destroyed on
 * any thread if the target thread has gone away.
 *
 * TODO(Bug 1991618): The holder always lives on the main thread, as does the
 * source. A source owned by a worker (e.g., a VideoTrackGenerator in a worker
 * whose track is transferred to the main thread or another worker) would need
 * the holder to live on that worker, kept alive with a ThreadSafeWorkerRef
 * while other threads may dispatch to it.
 */
class MediaStreamTrackSourceHandle final : public GraphTrackHolder::Owner {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MediaStreamTrackSourceHandle)

  /**
   * Receives notifications from the holder on the thread registered with
   * AddListener().
   */
  class Listener {
   public:
    NS_INLINE_DECL_PURE_VIRTUAL_REFCOUNTING

    virtual void MutedChanged(bool aNewState) = 0;
    /**
     * aSettings is a snapshot of the source's settings taken when the
     * constraints changed, since they cannot be queried off the main thread.
     */
    virtual void ConstraintsChanged(const MediaTrackConstraints& aConstraints,
                                    const MediaTrackSettings& aSettings) = 0;
    virtual void OverrideEnded() = 0;
    /**
     * Called with the holder's graph track once it exists, right after
     * registering or when a pending clone has been created. The track may be
     * borrowed until OverrideEnded().
     */
    virtual void GraphTrackAvailable(ProcessedMediaTrack* aTrack) = 0;

   protected:
    virtual ~Listener() = default;
  };

  /**
   * Creates a handle holding aSource and aInputTrack, see
   * GraphTrackHolder::Create. Main thread only. The handle starts out with no
   * users and aEnabled as its enabled state, so the caller must add a user
   * before returning to the event loop or the holder is destroyed.
   */
  static already_AddRefed<MediaStreamTrackSourceHandle> Create(
      MediaStreamTrackSource* aSource, mozilla::MediaTrack* aInputTrack,
      bool aEnabled);

  /**
   * Creates a pending handle whose holder is a clone of aOriginal's holder,
   * made on the main thread. Any thread. The handle has one user with the
   * given enabled state until the caller adds its own and removes this one.
   */
  static already_AddRefed<MediaStreamTrackSourceHandle> CreateClone(
      MediaStreamTrackSourceHandle* aOriginal, bool aEnabled);

  /**
   * True if the current thread is the holder's thread, on which Holder() may
   * be used.
   */
  bool IsOnSourceThread() const;

  // Main thread only. Null while a clone is pending, or after the last user
  // went away.
  GraphTrackHolder* Holder() const;

  /**
   * Adds a user of the holder. aEnabled tells whether the user wants the
   * source enabled. The holder's enabled state aggregates this over all users
   * and listeners. Any thread.
   */
  void AddUser(bool aEnabled);

  /**
   * Removes a user added with AddUser(), with the same aEnabled. Any thread.
   */
  void RemoveUser(bool aEnabled);

  /**
   * Registers aListener for notifications, dispatched to aTarget. The listener
   * counts as a user with the given enabled state. If the holder has already
   * ended, OverrideEnded() is dispatched right away, and if its graph track
   * exists, so is GraphTrackAvailable(). Any thread.
   */
  void AddListener(Listener* aListener, nsISerialEventTarget* aTarget,
                   bool aEnabled);

  /**
   * Unregisters aListener. Notifications already dispatched may still be
   * delivered, so listeners must handle that. Any thread.
   */
  void RemoveListener(Listener* aListener);

  /**
   * Updates the enabled state of a registered listener. Any thread.
   */
  void SetListenerEnabled(Listener* aListener, bool aEnabled);

  // GraphTrackHolder::Owner, main thread only.
  void HolderMutedChanged(bool aMuted) override;
  void HolderConstraintsChanged(const MediaTrackConstraints& aConstraints,
                                const MediaTrackSettings& aSettings) override;
  void HolderEnded() override;

 private:
  struct ListenerEntry {
    RefPtr<Listener> mListener;
    nsCOMPtr<nsISerialEventTarget> mTarget;
    bool mEnabled;
  };

  MediaStreamTrackSourceHandle();
  ~MediaStreamTrackSourceHandle();

  // Main thread only.
  void SetHolder(already_AddRefed<GraphTrackHolder> aHolder);
  void InitializeClone(MediaStreamTrackSourceHandle* aOriginal);
  void ApplyEnabled();
  void ReleaseHolder();
  bool AnyUserEnabled() const;
  template <typename Function>
  void ForwardToListeners(const char* aName, Function&& aFunction);
  template <typename Function>
  static void DispatchToListener(const ListenerEntry& aEntry, const char* aName,
                                 const Function& aFunction);

  // Any thread.
  bool AnyEnabledLocked() const MOZ_REQUIRES(mMutex);
  void OnUserRemoved(bool aWasEnabled);
  void NotifyEnabledStateChanged();
  template <typename Function>
  static void DispatchToMainThread(const char* aName, Function&& aFunction);

  // Main thread only. Null while a clone is pending, and after
  // ReleaseHolder(). Shut down and released on the main thread.
  RefPtr<GraphTrackHolder> mHolder;
  // Main thread only. Set once the last user is gone, so that a clone
  // finishing creation afterwards is released right away.
  bool mReleased = false;

  mutable Mutex mMutex;
  nsTArray<ListenerEntry> mListeners MOZ_GUARDED_BY(mMutex);
  // Users added with AddUser(), not counting listeners.
  uint32_t mUsers MOZ_GUARDED_BY(mMutex) = 0;
  uint32_t mEnabledUsers MOZ_GUARDED_BY(mMutex) = 0;
  // Set when the holder has ended, or cloning it failed.
  bool mEnded MOZ_GUARDED_BY(mMutex) = false;
  // The holder's graph track, for listeners to borrow. Set once the holder
  // exists and is live. Strong but not owning; the holder destroys it.
  RefPtr<ProcessedMediaTrack> mGraphTrack MOZ_GUARDED_BY(mMutex);
};

}  // namespace dom
}  // namespace mozilla

#endif  // DOM_MEDIA_MEDIASTREAMTRACKSOURCEHANDLE_H_
