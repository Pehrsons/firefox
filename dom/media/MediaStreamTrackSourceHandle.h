/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_MEDIASTREAMTRACKSOURCEHANDLE_H_
#define DOM_MEDIA_MEDIASTREAMTRACKSOURCEHANDLE_H_

#include "MediaTrackConstraints.h"
#include "PrincipalHandle.h"
#include "mozilla/Mutex.h"
#include "mozilla/RefPtr.h"
#include "mozilla/UniquePtr.h"
#include "nsCOMPtr.h"
#include "nsISerialEventTarget.h"
#include "nsISupportsImpl.h"
#include "nsTArray.h"

namespace mozilla {

class MediaTrack;

namespace dom {

class MediaStreamTrackSource;

/**
 * A thread-safe handle to a MediaStreamTrackSource. It allows
 * MediaStreamTracks living on other threads than the source (i.e., in
 * dedicated workers) to be tied to the source as if they were created in the
 * source's context, per the MediaStreamTrack transfer steps in
 * https://w3c.github.io/mediacapture-extensions/#transferable-mediastreamtrack
 *
 * The handle is dataHolder.[[source]] in those steps. It is created on the
 * source's thread and registers a MediaStreamTrackSource::Sink that keeps the
 * source alive for as long as the handle has users. A user is either a
 * MediaStreamTrack::TransferredData in flight, or a TransferredTrackSource on
 * the receiving thread. When the last user goes away the sink is unregistered,
 * which lets the source stop.
 *
 * Notifications from the source are forwarded to all registered Listeners by
 * dispatching to their respective threads. Listeners must be thread-safe
 * refcounted since the dispatched runnables hold them, and may be destroyed on
 * any thread if the target thread has gone away.
 *
 * TODO(Bug 1991619): Only sources owned by the main thread are supported. A
 * source owned by a worker (e.g., a VideoTrackGenerator in a worker whose
 * track is transferred to the main thread or another worker) requires the
 * handle to keep that worker alive with a ThreadSafeWorkerRef while other
 * threads may dispatch to it, and to stop dispatching once the worker is
 * shutting down.
 */
class MediaStreamTrackSourceHandle final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MediaStreamTrackSourceHandle)

  /**
   * Receives notifications from the source on the thread registered with
   * AddListener().
   */
  class Listener {
   public:
    NS_INLINE_DECL_PURE_VIRTUAL_REFCOUNTING

    virtual void PrincipalChanged(const PrincipalHandle& aPrincipalHandle) = 0;
    virtual void MutedChanged(bool aNewState) = 0;
    virtual void ConstraintsChanged(
        const MediaTrackConstraints& aConstraints) = 0;
    virtual void OverrideEnded() = 0;

   protected:
    virtual ~Listener() = default;
  };

  /**
   * Creates a handle to aSource. Must be called on the source's thread. The
   * handle starts out with no users, so the caller must add one before
   * returning to the event loop or the source may stop.
   *
   * aInputTrack is the MediaTrack the source feeds the transferred track with.
   * TODO(Bug 1991619): This is unused until tracks on other threads get a
   * MediaTrackGraph representation.
   */
  static already_AddRefed<MediaStreamTrackSourceHandle> Create(
      MediaStreamTrackSource* aSource, mozilla::MediaTrack* aInputTrack);

  /**
   * True if the current thread is the source's thread, on which Source() and
   * InputTrack() may be used directly.
   */
  bool IsOnSourceThread() const;

  // Source thread only.
  MediaStreamTrackSource* Source() const;
  mozilla::MediaTrack* InputTrack() const;

  /**
   * Adds a user of the source. aEnabled tells whether the user wants the
   * source enabled. The sink registered with the source aggregates this over
   * all users and listeners. Any thread.
   */
  void AddUser(bool aEnabled);

  /**
   * Removes a user added with AddUser(), with the same aEnabled. Any thread.
   */
  void RemoveUser(bool aEnabled);

  /**
   * Registers aListener for notifications, dispatched to aTarget. The listener
   * counts as a user with the given enabled state. Any thread.
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

 private:
  class KeepAliveSink;
  friend class KeepAliveSink;

  struct ListenerEntry {
    RefPtr<Listener> mListener;
    nsCOMPtr<nsISerialEventTarget> mTarget;
    bool mEnabled;
  };

  MediaStreamTrackSourceHandle(MediaStreamTrackSource* aSource,
                               mozilla::MediaTrack* aInputTrack);
  ~MediaStreamTrackSourceHandle();

  // Source thread only.
  void RegisterSink();
  void UnregisterSink();
  bool AnyUserEnabled() const;
  void ForwardPrincipalChanged();
  void ForwardMutedChanged(bool aNewState);
  void ForwardConstraintsChanged(const MediaTrackConstraints& aConstraints);
  void ForwardOverrideEnded();
  template <typename Function>
  void ForwardToListeners(const char* aName, Function&& aFunction);

  // Any thread.
  bool AnyEnabledLocked() const MOZ_REQUIRES(mMutex);
  void OnUserRemoved(bool aWasEnabled);
  void NotifySourceEnabledStateChanged();

  // Source thread only. Released on the source thread in the destructor.
  RefPtr<MediaStreamTrackSource> mSource;
  const RefPtr<mozilla::MediaTrack> mInputTrack;
  // Source thread only. Registered with mSource while there are users. Null
  // once unregistered.
  UniquePtr<KeepAliveSink> mSink;

  mutable Mutex mMutex;
  nsTArray<ListenerEntry> mListeners MOZ_GUARDED_BY(mMutex);
  // Users added with AddUser(), not counting listeners.
  uint32_t mUsers MOZ_GUARDED_BY(mMutex) = 0;
  uint32_t mEnabledUsers MOZ_GUARDED_BY(mMutex) = 0;
};

}  // namespace dom
}  // namespace mozilla

#endif  // DOM_MEDIA_MEDIASTREAMTRACKSOURCEHANDLE_H_
