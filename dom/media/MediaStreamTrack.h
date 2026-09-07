/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MEDIASTREAMTRACK_H_
#define MEDIASTREAMTRACK_H_

#include "MediaEngineSource.h"
#include "MediaSegment.h"
#include "MediaTrackConstraints.h"
#include "PerformanceRecorder.h"
#include "PrincipalChangeObserver.h"
#include "PrincipalHandle.h"
#include "mozilla/DOMEventTargetHelper.h"
#include "mozilla/StateMirroring.h"
#include "mozilla/StateWatching.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/WeakPtr.h"
#include "mozilla/dom/MediaStreamTrackBinding.h"
#include "mozilla/dom/MediaTrackCapabilitiesBinding.h"
#include "mozilla/dom/MediaTrackSettingsBinding.h"
#include "mozilla/media/MediaUtils.h"
#include "nsError.h"
#include "nsID.h"
#include "nsIPrincipal.h"

namespace mozilla {

class DOMMediaStream;
class MediaEnginePhotoCallback;
class MediaInputPort;
class MediaTrack;
class MediaTrackGraph;
class MediaTrackGraphImpl;
class MediaTrackListener;
class DirectMediaTrackListener;
class PeerConnectionImpl;
class PeerIdentity;
class ProcessedMediaTrack;
class RemoteSourceStreamInfo;
class SourceStreamInfo;
class MediaMgrError;

namespace dom {

class AudioStreamTrack;
class GraphTrackHolder;
class VideoStreamTrack;
class MediaStreamTrackSourceHandle;
class RTCStatsTimestampMaker;
enum class CallerType : uint32_t;

using MediaStreamTrackSourceSettings = MediaEngineSourceSettings;
using MediaStreamTrackSourceCapabilities = MediaEngineSourceCapabilities;

/**
 * Common interface through which a MediaStreamTrack can communicate with its
 * producer on the thread owning the track.
 *
 * Sources of tracks created on the main thread live on the main thread. A
 * track transferred to a worker gets a TransferredTrackSource living on the
 * worker thread, which proxies to the original source through a
 * MediaStreamTrackSourceHandle.
 *
 * Kept alive by a strong ref in all MediaStreamTracks (original and clones)
 * sharing this source.
 */
class MediaStreamTrackSource : public nsISupports {
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_CLASS(MediaStreamTrackSource)

 public:
  class Sink : public SupportsWeakPtr {
   public:
    /**
     * Must be constant throughout the Sink's lifetime.
     *
     * Return true to keep the MediaStreamTrackSource where this sink is
     * registered alive.
     * Return false to allow the source to stop.
     *
     * Typically MediaStreamTrack::Sink returns true and other Sinks
     * (like HTMLMediaElement::StreamCaptureTrackSource) return false.
     */
    virtual bool KeepsSourceAlive() const = 0;

    /**
     * Return true to ensure that the MediaStreamTrackSource where this Sink is
     * registered is kept turned on and active.
     * Return false to allow the source to pause, and any underlying devices to
     * temporarily stop.
     *
     * When the underlying enabled state of the sink changes,
     * call MediaStreamTrackSource::SinkEnabledStateChanged().
     *
     * Typically MediaStreamTrack returns the track's enabled state and other
     * Sinks (like HTMLMediaElement::StreamCaptureTrackSource) return false so
     * control over device state remains with tracks and their enabled state.
     */
    virtual bool Enabled() const = 0;

    /**
     * Called when the principal of the MediaStreamTrackSource where this sink
     * is registered has changed.
     */
    virtual void PrincipalChanged() = 0;

    /**
     * Called when the muted state of the MediaStreamTrackSource where this sink
     * is registered has changed.
     */
    virtual void MutedChanged(bool aNewState) = 0;

    /**
     * Called when the constraints of the MediaStreamTrackSource where this sink
     * is registered has changed.
     */
    virtual void ConstraintsChanged(
        const MediaTrackConstraints& aConstraints) = 0;

    /**
     * Called when the MediaStreamTrackSource where this sink is registered has
     * stopped producing data for good, i.e., it has ended.
     */
    virtual void OverrideEnded() = 0;

   protected:
    virtual ~Sink() = default;
  };

  MediaStreamTrackSource(nsIPrincipal* aPrincipal, const nsString& aLabel,
                         TrackingId aTrackingId)
      : MediaStreamTrackSource(aPrincipal, aLabel, std::move(aTrackingId),
                               nsString(), nsString()) {}

  // The Canonicals are owned by AbstractThread::GetCurrent(), which must exist.
  MediaStreamTrackSource(nsIPrincipal* aPrincipal, const nsString& aLabel,
                         TrackingId aTrackingId, const nsString& aDeviceId,
                         const nsString& aGroupId)
      : mPrincipal(aPrincipal),
        mLabel(aLabel),
        mTrackingId(std::move(aTrackingId)),
        mDeviceId(aDeviceId),
        mGroupId(aGroupId),
        mSettings(AbstractThread::GetCurrent(),
                  MediaStreamTrackSourceSettings(),
                  "MediaStreamTrackSource::mSettings"),
        mCapabilities(AbstractThread::GetCurrent(),
                      MediaStreamTrackSourceCapabilities(),
                      "MediaStreamTrackSource::mCapabilities"),
        mStopped(false) {}

  /**
   * Use to clean up any resources that have to be cleaned before the
   * destructor is called. It is often too late in the destructor because
   * of garbage collection having removed the members already.
   */
  virtual void Destroy() {}

  struct CloneResult {
    ~CloneResult();

    RefPtr<MediaStreamTrackSource> mSource;
    RefPtr<mozilla::MediaTrack> mInputTrack;
  };

  /**
   * Clone this MediaStreamTrackSource. Cloned sources allow independent track
   * settings. Not supported by all source types. A source not supporting
   * cloning returns nullptr.
   */
  virtual CloneResult Clone();

  /**
   * Gets the source's MediaSourceEnum for usage by PeerConnections.
   */
  virtual MediaSourceEnum GetMediaSource() const = 0;

  /**
   * Get this TrackSource's principal.
   */
  nsIPrincipal* GetPrincipal() const { return mPrincipal; }

  /**
   * This is used in WebRTC. A peerIdentity constrained MediaStreamTrack cannot
   * be sent across the network to anything other than a peer with the provided
   * identity. If this is set, then GetPrincipal() should return an instance of
   * NullPrincipal.
   *
   * A track's PeerIdentity is immutable and will not change during the track's
   * lifetime.
   */
  virtual const PeerIdentity* GetPeerIdentity() const { return nullptr; }

  /**
   * This is used in WebRTC. The timestampMaker can convert between different
   * timestamp types used during the session.
   */
  virtual const RTCStatsTimestampMaker* GetTimestampMaker() const {
    return nullptr;
  }

  /**
   * MediaStreamTrack::GetLabel (see spec) calls through to here.
   */
  void GetLabel(nsAString& aLabel) { aLabel.Assign(mLabel); }

  /**
   * Whether this TrackSource provides video frames with an alpha channel. Only
   * applies to video sources. Used by HTMLVideoElement.
   */
  virtual bool HasAlpha() const { return false; }

  /**
   * Forwards a photo request to backends that support it. Other backends return
   * NS_ERROR_NOT_IMPLEMENTED to indicate that a MediaTrackGraph-based fallback
   * should be used.
   */
  virtual nsresult TakePhoto(MediaEnginePhotoCallback*) const {
    return NS_ERROR_NOT_IMPLEMENTED;
  }

  typedef MozPromise<bool /* aIgnored */, RefPtr<MediaMgrError>, false>
      ApplyConstraintsPromise;

  /**
   * We provide a fallback solution to ApplyConstraints() here.
   * Sources that support ApplyConstraints() will have to override it.
   */
  virtual RefPtr<ApplyConstraintsPromise> ApplyConstraints(
      const dom::MediaTrackConstraints& aConstraints, CallerType aCallerType);

  /**
   * The settings and capabilities of this source, owned by the thread it was
   * created on. Subclasses set them as their state changes so that mirrors on
   * other threads stay up to date.
   */
  AbstractCanonical<MediaStreamTrackSourceSettings>& CanonicalSettings() {
    return mSettings;
  }
  AbstractCanonical<MediaStreamTrackSourceCapabilities>&
  CanonicalCapabilities() {
    return mCapabilities;
  }
  const MediaStreamTrackSourceSettings& Settings() const {
    return mSettings.Ref();
  }
  const MediaStreamTrackSourceCapabilities& Capabilities() const {
    return mCapabilities.Ref();
  }

  /**
   * Returns the thread-safe handle this source proxies to, if it is a
   * TransferredTrackSource, so that a track transferred multiple times stays
   * tied to the original source only. Sources living on the main thread return
   * null, and the track being transferred creates a handle holding this
   * source instead. See MediaStreamTrackSourceHandle.
   */
  virtual already_AddRefed<MediaStreamTrackSourceHandle> TransferHandle() {
    return nullptr;
  }

  /**
   * Called by the source interface when all registered sinks with
   * KeepsSourceAlive() == true have unregistered.
   */
  virtual void Stop() = 0;

  /**
   * Called by the source interface when all registered sinks with
   * KeepsSourceAlive() == true become disabled.
   */
  virtual void Disable() = 0;

  /**
   * Called by the source interface when at least one registered sink with
   * KeepsSourceAlive() == true become enabled.
   */
  virtual void Enable() = 0;

  /**
   * Called when a Sink's Enabled() state changed. Will iterate through all
   * sinks and notify the source of the aggregated enabled state.
   *
   * Note that a Sink with KeepsSourceAlive() == false counts as disabled.
   */
  void SinkEnabledStateChanged() {
    if (IsEnabled()) {
      Enable();
    } else {
      Disable();
    }
  }

  /**
   * Called by each MediaStreamTrack clone on initialization.
   */
  void RegisterSink(Sink* aSink) {
    NS_ASSERT_OWNINGTHREAD(MediaStreamTrackSource);
    if (mStopped) {
      return;
    }
    mSinks.RemoveElementsBy([](const WeakPtr<Sink>& aElem) {
      MOZ_ASSERT(aElem, "Sink was not explicitly removed");
      return !aElem;
    });
    mSinks.AppendElement(aSink);
  }

  /**
   * Called by each MediaStreamTrack clone on Stop() if supported by the
   * source (us) or destruction.
   */
  void UnregisterSink(Sink* aSink) {
    NS_ASSERT_OWNINGTHREAD(MediaStreamTrackSource);
    mSinks.RemoveElementsBy([](const WeakPtr<Sink>& aElem) {
      MOZ_ASSERT(aElem, "Sink was not explicitly removed");
      return !aElem;
    });
    if (mSinks.RemoveElement(aSink) && !IsActive()) {
      MOZ_ASSERT(!aSink->KeepsSourceAlive() || !mStopped,
                 "When the last sink keeping the source alive is removed, "
                 "we should still be live");
      Stop();
      mStopped = true;
    }
    if (!mStopped) {
      SinkEnabledStateChanged();
    }
  }

 protected:
  virtual ~MediaStreamTrackSource() = default;

  bool IsActive() {
    for (const WeakPtr<Sink>& sink : mSinks) {
      if (sink && sink->KeepsSourceAlive()) {
        return true;
      }
    }
    return false;
  }

  bool IsEnabled() {
    for (const WeakPtr<Sink>& sink : mSinks) {
      if (sink && sink->KeepsSourceAlive() && sink->Enabled()) {
        return true;
      }
    }
    return false;
  }

  /**
   * Called by a sub class when the principal has changed.
   * Notifies all sinks.
   */
  void PrincipalChanged() {
    NS_ASSERT_OWNINGTHREAD(MediaStreamTrackSource);
    mSinks.RemoveElementsBy([](const WeakPtr<Sink>& aElem) {
      MOZ_ASSERT(aElem, "Sink was not explicitly removed");
      return !aElem;
    });
    for (const auto& sink : mSinks.Clone()) {
      sink->PrincipalChanged();
    }
  }

  /**
   * Called by a sub class when the source's muted state has changed. Note that
   * the source is responsible for making the content black/silent during mute.
   * Notifies all sinks.
   */
  void MutedChanged(bool aNewState) {
    NS_ASSERT_OWNINGTHREAD(MediaStreamTrackSource);
    mSinks.RemoveElementsBy([](const WeakPtr<Sink>& aElem) {
      MOZ_ASSERT(aElem, "Sink was not explicitly removed");
      return !aElem;
    });
    for (const auto& sink : mSinks.Clone()) {
      sink->MutedChanged(aNewState);
    }
  }

  /**
   * Called by a sub class when the source's applied constraints has changed.
   * Notifies all sinks.
   */
  void ConstraintsChanged(const MediaTrackConstraints& aConstraints) {
    NS_ASSERT_OWNINGTHREAD(MediaStreamTrackSource);
    mSinks.RemoveElementsBy([](const WeakPtr<Sink>& aElem) {
      MOZ_ASSERT(aElem, "Sink was not explicitly removed");
      return !aElem;
    });
    for (const auto& sink : mSinks.Clone()) {
      sink->ConstraintsChanged(aConstraints);
    }
  }

  /**
   * Called by a sub class when the source has stopped producing data for good,
   * i.e., it has ended. Notifies all sinks.
   */
  void OverrideEnded() {
    NS_ASSERT_OWNINGTHREAD(MediaStreamTrackSource);
    mSinks.RemoveElementsBy([](const WeakPtr<Sink>& aElem) {
      MOZ_ASSERT(aElem, "Sink was not explicitly removed");
      return !aElem;
    });
    for (const auto& sink : mSinks.Clone()) {
      sink->OverrideEnded();
    }
  }

  // Principal identifying who may access the contents of this source.
  RefPtr<nsIPrincipal> mPrincipal;

  // Currently registered sinks.
  nsTArray<WeakPtr<Sink>> mSinks;

 public:
  // The label of the track we are the source of per the MediaStreamTrack spec.
  const nsString mLabel;

  // Set for all video sources; an id for tracking the source of the video
  // frames for this track.
  const TrackingId mTrackingId;

  // The deviceId and groupId settings. Empty for sources that don't expose
  // them.
  const nsString mDeviceId;
  const nsString mGroupId;

 protected:
  Canonical<MediaStreamTrackSourceSettings> mSettings;
  Canonical<MediaStreamTrackSourceCapabilities> mCapabilities;

  // True if all MediaStreamTrack users have unregistered from this source and
  // Stop() has been called.
  bool mStopped;
};

/**
 * Base class that consumers of a MediaStreamTrack can use to get notifications
 * about state changes in the track.
 */
class MediaStreamTrackConsumer : public SupportsWeakPtr {
 public:
  /**
   * Called when the track's readyState transitions to "ended".
   * Unlike the "ended" event exposed to script this is called for any reason,
   * including MediaStreamTrack::Stop().
   */
  virtual void NotifyEnded(MediaStreamTrack* aTrack) {};

  /**
   * Called when the track's enabled state changes.
   */
  virtual void NotifyEnabledChanged(MediaStreamTrack* aTrack, bool aEnabled) {};
};

// clang-format off
/**
 * DOM wrapper for MediaTrackGraph-MediaTracks.
 *
 * To account for cloning, a MediaStreamTrack wraps two internal (and chained)
 * MediaTracks, held by its GraphTrackHolder:
 *   1. The input track
 *      - Controlled by the producer of the data in the track. The producer
 *        decides on lifetime of the MediaTrack and the track inside it.
 *      - It can be any type of MediaTrack.
 *      - Contains one track only.
 *   2. mTrack
 *      - A ForwardedInputTrack representing this MediaStreamTrack.
 *      - Its data is piped from the input track through a port.
 *      - Contains one track only.
 *      - When this MediaStreamTrack is enabled/disabled this is reflected in
 *        the chunks in the track in mTrack.
 *      - When this MediaStreamTrack has ended, mTrack gets destroyed.
 *        Note that the input track is unaffected, such that any clones of
 *        mTrack can live on. When all clones are ended, this is signaled to the
 *        producer via our MediaStreamTrackSource. It is then likely to destroy
 *        the input track.
 *
 * A graphical representation of how tracks are connected when cloned follows:
 *
 * MediaStreamTrack A
 *       input track     mTrack
 *            t1 ---------> t1
 *               \
 *                -----
 * MediaStreamTrack B  \  (clone of A)
 *       input track   \ mTrack
 *            *          -> t1
 *
 *   (*) is a copy of A's input track
 *
 * Only the main thread creates and destroys graph objects. When a track is
 * transferred to a dedicated worker, its holder moves to a
 * MediaStreamTrackSourceHandle on the main thread.
 */
// clang-format on
class MediaStreamTrack : public DOMEventTargetHelper, public SupportsWeakPtr {
  // PeerConnection and friends need to know our owning DOMStream and track id.
  friend class mozilla::PeerConnectionImpl;
  friend class mozilla::SourceStreamInfo;
  friend class mozilla::RemoteSourceStreamInfo;

  class TrackSink;

 public:
  /**
   * The data holder of the MediaStreamTrack transfer steps, see
   * https://w3c.github.io/mediacapture-extensions/#transferable-mediastreamtrack
   *
   * Created by Transfer() on the transferring thread and consumed by
   * FromTransferred() on the receiving thread. May be destroyed on any thread.
   * Keeps the underlying source alive while it exists.
   */
  struct TransferredData final {
    TransferredData(const nsAString& aId, MediaSegment::Type aKind,
                    const nsAString& aLabel, MediaStreamTrackState aReadyState,
                    bool aEnabled, bool aMuted,
                    const MediaTrackConstraints& aConstraints,
                    const MediaStreamTrackSourceSettings& aSettings,
                    const MediaStreamTrackSourceCapabilities& aCapabilities,
                    const nsAString& aDeviceId, const nsAString& aGroupId,
                    MediaSourceEnum aMediaSource, bool aHasAlpha,
                    RefPtr<MediaStreamTrackSourceHandle> aSource);
    ~TransferredData();

    // [[id]]
    const nsString mId;
    // [[kind]]
    const MediaSegment::Type mKind;
    // [[label]]
    const nsString mLabel;
    // [[readyState]]
    const MediaStreamTrackState mReadyState;
    // [[enabled]]
    const bool mEnabled;
    // [[muted]]
    const bool mMuted;
    // [[constraints]]
    const MediaTrackConstraints mConstraints;
    // [[contentHint]] is not implemented.

    // Snapshots of source state for a track on another thread than the
    // source, until its Mirrors of the source's Canonicals catch up.
    const MediaStreamTrackSourceSettings mSettings;
    const MediaStreamTrackSourceCapabilities mCapabilities;
    const nsString mDeviceId;
    const nsString mGroupId;
    const MediaSourceEnum mMediaSource;
    const bool mHasAlpha;

    // [[source]]
    const RefPtr<MediaStreamTrackSourceHandle> mSource;
  };

  MediaStreamTrack(
      nsIGlobalObject* aGlobal, mozilla::MediaTrack* aInputTrack,
      MediaStreamTrackSource* aSource,
      MediaStreamTrackState aReadyState = MediaStreamTrackState::Live,
      bool aMuted = false,
      const MediaTrackConstraints& aConstraints = MediaTrackConstraints());

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(MediaStreamTrack,
                                           DOMEventTargetHelper)

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  nsIGlobalObject* GetParentObject() const { return mGlobal; }
  // The window of mGlobal, or null for other globals.
  nsGlobalWindowInner* GetOwnerWindow() const;

  /**
   * WebIDL Func for MediaStreamTrack and MediaStreamTrackEvent, and through
   * DOMMediaStream::IsExposed for MediaStream.
   * Always exposed in Window. Exposed in DedicatedWorker only when the pref
   * media.mediastreamtrack.transferable.enabled is set.
   */
  static bool IsExposed(JSContext* aCx, JSObject* aGlobal);

  /**
   * The MediaStreamTrack transfer steps. Returns nullptr if the track is
   * detached, in which case the caller throws a DataCloneError. Detaches this
   * track and sets its readyState to "ended" without stopping the source.
   */
  UniquePtr<TransferredData> Transfer();

  /**
   * The MediaStreamTrack transfer-receiving steps. Creates a track in aGlobal
   * tied to the source of the transferred track. Returns nullptr if the
   * receiving worker is shutting down.
   */
  static already_AddRefed<MediaStreamTrack> FromTransferred(
      nsIGlobalObject* aGlobal, const TransferredData& aData);

  virtual AudioStreamTrack* AsAudioStreamTrack() { return nullptr; }
  virtual VideoStreamTrack* AsVideoStreamTrack() { return nullptr; }

  virtual const AudioStreamTrack* AsAudioStreamTrack() const { return nullptr; }
  virtual const VideoStreamTrack* AsVideoStreamTrack() const { return nullptr; }

  // WebIDL
  virtual void GetKind(nsAString& aKind) = 0;
  void GetId(nsAString& aID) const;
  virtual void GetLabel(nsAString& aLabel, CallerType /* aCallerType */) {
    GetSource().GetLabel(aLabel);
  }
  bool Enabled() const { return mEnabled; }
  void SetEnabled(bool aEnabled);
  bool Muted() { return mMuted; }
  void Stop();
  void GetCapabilities(MediaTrackCapabilities& aResult, CallerType aCallerType);
  void GetConstraints(dom::MediaTrackConstraints& aResult);
  void GetSettings(dom::MediaTrackSettings& aResult, CallerType aCallerType);

  already_AddRefed<Promise> ApplyConstraints(
      const dom::MediaTrackConstraints& aConstraints, CallerType aCallerType,
      ErrorResult& aRv);
  virtual already_AddRefed<MediaStreamTrack> Clone() = 0;
  MediaStreamTrackState ReadyState() { return mReadyState; }

  IMPL_EVENT_HANDLER(mute)
  IMPL_EVENT_HANDLER(unmute)
  IMPL_EVENT_HANDLER(ended)

  /**
   * Convenience (and legacy) method for when ready state is "ended".
   */
  bool Ended() const { return mReadyState == MediaStreamTrackState::Ended; }

  /**
   * Get this track's principal.
   */
  nsIPrincipal* GetPrincipal() const { return mPrincipal; }

  /**
   * Get this track's PeerIdentity.
   */
  const PeerIdentity* GetPeerIdentity() const {
    return GetSource().GetPeerIdentity();
  }

  /**
   * Get this track's RTCStatsTimestampMaker.
   */
  const RTCStatsTimestampMaker* GetTimestampMaker() const {
    return GetSource().GetTimestampMaker();
  }

  ProcessedMediaTrack* GetTrack() const;
  MediaTrackGraph* Graph() const;
  MediaTrackGraphImpl* GraphImpl() const;

  MediaStreamTrackSource& GetSource() const {
    MOZ_RELEASE_ASSERT(mSource,
                       "The track source is only removed on destruction");
    return *mSource;
  }

  // Webrtc allows the remote side to name tracks whatever it wants, and we
  // need to surface this to content.
  void AssignId(const nsAString& aID) { mID = aID; }

  /**
   * Add a PrincipalChangeObserver to this track.
   *
   * Returns true if it was successfully added.
   *
   * Ownership of the PrincipalChangeObserver remains with the caller, and it's
   * the caller's responsibility to remove the observer before it dies.
   */
  bool AddPrincipalChangeObserver(
      PrincipalChangeObserver<MediaStreamTrack>* aObserver);

  /**
   * Remove an added PrincipalChangeObserver from this track.
   *
   * Returns true if it was successfully removed.
   */
  bool RemovePrincipalChangeObserver(
      PrincipalChangeObserver<MediaStreamTrack>* aObserver);

  /**
   * Add a MediaStreamTrackConsumer to this track.
   *
   * Adding the same consumer multiple times is prohibited.
   */
  void AddConsumer(MediaStreamTrackConsumer* aConsumer);

  /**
   * Remove an added MediaStreamTrackConsumer from this track.
   */
  void RemoveConsumer(MediaStreamTrackConsumer* aConsumer);

  /**
   * Adds a MediaTrackListener to the MediaTrackGraph representation of
   * this track.
   */
  virtual void AddListener(MediaTrackListener* aListener);

  /**
   * Removes a MediaTrackListener from the MediaTrackGraph representation
   * of this track.
   */
  void RemoveListener(MediaTrackListener* aListener);

  /**
   * Attempts to add a direct track listener to this track.
   * Callers must listen to the NotifyInstalled event to know if installing
   * the listener succeeded (tracks originating from SourceMediaTracks) or
   * failed (e.g., WebAudio originated tracks).
   */
  virtual void AddDirectListener(DirectMediaTrackListener* aListener);
  void RemoveDirectListener(DirectMediaTrackListener* aListener);

  /**
   * Sets up a MediaInputPort from the underlying track that this
   * MediaStreamTrack represents, to aTrack, and returns it.
   */
  already_AddRefed<MediaInputPort> ForwardTrackContentsTo(
      ProcessedMediaTrack* aTrack);

 protected:
  virtual ~MediaStreamTrack();

  /**
   * Forces the ready state to a particular value, for instance when we're
   * cloning an already ended track.
   */
  virtual void SetReadyState(MediaStreamTrackState aState);

  /**
   * Notified by the MediaTrackGraph, through our owning MediaStream on the
   * main thread.
   *
   * Note that this sets the track to ended and raises the "ended" event
   * synchronously.
   */
  void OverrideEnded();

  /**
   * Called on the main thread when mTrackPrincipalHandle changes, i.e., when
   * the PrincipalHandle of the data in mTrack has changed in the
   * MediaTrackGraph. When it matches the pending principal we know that the
   * principal change has propagated to consumers.
   */
  void OnPrincipalHandleChanged();

  /**
   * Called on the main thread when mTrackEnded changes, i.e., when mTrack has
   * ended in the MediaTrackGraph. Queues a task to end this track.
   */
  void OnTrackEnded();

  /**
   * Starts borrowing aTrack as mTrack and mirroring its state.
   */
  void SetGraphTrack(ProcessedMediaTrack* aTrack);

  // The input track of our holder, for cloning. Null without a holder.
  mozilla::MediaTrack* InputTrack() const;

  /**
   * Called when this track's readyState transitions to "ended".
   * Notifies all MediaStreamTrackConsumers that this track ended.
   */
  void NotifyEnded();

  /**
   * Called when this track's enabled state has changed.
   * Notifies all MediaStreamTrackConsumers.
   */
  void NotifyEnabledChanged();

  /**
   * Called when mSource's principal has changed.
   */
  void PrincipalChanged();

  /**
   * Called when mSource's muted state has changed.
   */
  void MutedChanged(bool aNewState);

  /**
   * Called when mSource's applied constraints has changed.
   */
  void ConstraintsChanged(const MediaTrackConstraints& aConstraints);

  /**
   * Sets this track's muted state without raising any events.
   * Only really set by cloning. See MutedChanged for runtime changes.
   */
  void SetMuted(bool aMuted) { mMuted = aMuted; }

  virtual void Destroy();

  /**
   * Sets the principal and notifies PrincipalChangeObservers if it changes.
   */
  void SetPrincipal(nsIPrincipal* aPrincipal);

  /**
   * Creates a new MediaStreamTrack with the same kind, input track, input
   * track ID and source as this MediaStreamTrack.
   */

  template <typename TrackType>
  already_AddRefed<MediaStreamTrack> CloneInternal() {
    auto cloneRes = mSource->Clone();
    MOZ_ASSERT(!!cloneRes.mSource == !!cloneRes.mInputTrack);
    if (!cloneRes.mSource || !cloneRes.mInputTrack) {
      cloneRes.mSource = mSource;
      cloneRes.mInputTrack = InputTrack();
    }
    auto newTrack = MakeRefPtr<TrackType>(
        GetParentObject(), cloneRes.mInputTrack, cloneRes.mSource, ReadyState(),
        Muted(), mConstraints);
    newTrack->SetEnabled(Enabled());
    newTrack->SetMuted(Muted());
    return newTrack.forget();
  }

  nsTArray<PrincipalChangeObserver<MediaStreamTrack>*>
      mPrincipalChangeObservers;

  nsTArray<WeakPtr<MediaStreamTrackConsumer>> mConsumers;

  // Our global. Held directly rather than as DOMEventTargetHelper's owner, so
  // that tearing down the global does not disconnect us from it: a track
  // keeps ending and firing events after its window has navigated away.
  nsCOMPtr<nsIGlobalObject> mGlobal;
  // Holds the input track assigned us by the data producer and owns mTrack.
  // Set on construction if we're live and on the main thread. Valid until we
  // end. Shut down and released when we end or are transferred.
  RefPtr<GraphTrackHolder> mHolder;
  // The MediaTrack representing this MediaStreamTrack in the MediaTrackGraph.
  // Borrowed from mHolder, which guarantees it is not destroyed while we
  // borrow it. Set on construction if we're live. Valid until we end.
  // TODO(Bug 1991619): Null for tracks transferred to a worker, which have no
  // MediaTrackGraph representation yet.
  RefPtr<ProcessedMediaTrack> mTrack;
  RefPtr<MediaStreamTrackSource> mSource;
  const UniquePtr<TrackSink> mSink;
  nsCOMPtr<nsIPrincipal> mPrincipal;
  nsCOMPtr<nsIPrincipal> mPendingPrincipal;
  // Keep tracking MediaTrackListener and DirectMediaTrackListener,
  // so we can remove them in |Destory|.
  nsTArray<RefPtr<MediaTrackListener>> mTrackListeners;
  nsTArray<RefPtr<DirectMediaTrackListener>> mDirectTrackListeners;
  nsString mID;
  MediaStreamTrackState mReadyState;
  bool mEnabled;
  bool mMuted;
  // [[IsDetached]] per the transfer steps. Set by Transfer().
  bool mIsDetached = false;
  dom::MediaTrackConstraints mConstraints;
  // The owning thread. The main thread, or a worker thread's AbstractThread,
  // see WorkerPrivate::GetWorkerAbstractThread().
  const RefPtr<AbstractThread> mAbstractThread;
  WatchManager<MediaStreamTrack> mWatchManager;
  // Mirrors mTrack's ended state from the MediaTrackGraph while we're live.
  Mirror<bool> mTrackEnded;
  // Mirrors the PrincipalHandle of mTrack's most recent data while we're live.
  Mirror<PrincipalHandle> mTrackPrincipalHandle;
};

}  // namespace dom
}  // namespace mozilla

#endif /* MEDIASTREAMTRACK_H_ */
