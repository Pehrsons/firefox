/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaEngineFake.h"

#include "AudioSegment.h"
#include "FakeVideoSource.h"
#include "ImageContainer.h"
#include "MediaEnginePrefs.h"
#include "MediaEngineSource.h"
#include "MediaManager.h"
#include "MediaTrackConstraints.h"
#include "MediaTrackGraph.h"
#include "MediaTrackListener.h"
#include "SineWaveGenerator.h"
#include "Tracing.h"
#include "VideoSegment.h"
#include "mozilla/MediaManager.h"
#include "mozilla/SyncRunnable.h"
#include "mozilla/UniquePtr.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"

#ifdef MOZ_WIDGET_ANDROID
#  include "nsISupportsUtils.h"
#endif

namespace mozilla {

using namespace mozilla::gfx;
using dom::GetEnumString;
using dom::MediaSourceEnum;
using dom::MediaTrackCapabilities;
using dom::MediaTrackConstraints;
using dom::MediaTrackSettings;
using dom::VideoFacingModeEnum;
using dom::VideoResizeModeEnum;

#ifdef DEBUG
static constexpr int VIDEO_WIDTH_DEFAULT =
    MediaEnginePrefs::DEFAULT_43_VIDEO_WIDTH / 2;
#else
static constexpr int VIDEO_WIDTH_DEFAULT =
    MediaEnginePrefs::DEFAULT_43_VIDEO_WIDTH;
#endif
static constexpr int VIDEO_WIDTH_MAX = 4096;
#ifdef DEBUG
static constexpr int VIDEO_HEIGHT_DEFAULT =
    MediaEnginePrefs::DEFAULT_43_VIDEO_HEIGHT / 2;
#else
static constexpr int VIDEO_HEIGHT_DEFAULT =
    MediaEnginePrefs::DEFAULT_43_VIDEO_HEIGHT;
#endif
static constexpr int VIDEO_HEIGHT_MAX = 2160;

static nsString FakeVideoName() {
  // For the purpose of testing we allow to change the name of the fake device
  // by pref.
  nsAutoString cameraNameFromPref;
  nsresult rv;
  auto getPref = [&]() {
    rv = Preferences::GetString("media.getusermedia.fake-camera-name",
                                cameraNameFromPref);
  };
  if (NS_IsMainThread()) {
    getPref();
  } else {
    // Here it is preferred a "hard" block, instead of "soft" block provided
    // by sync dispatch, which allows the waiting thread to spin its event
    // loop. The latter would allow multiple enumeration requests being
    // processed out-of-order.
    RefPtr runnable = NS_NewRunnableFunction(__func__, getPref);
    SyncRunnable::DispatchToThread(GetMainThreadSerialEventTarget(), runnable);
  }

  if (NS_SUCCEEDED(rv)) {
    return std::move(cameraNameFromPref);
  }
  return u"Default Video Device"_ns;
}

/**
 * Fake video source.
 */
class MediaEngineFakeVideoSource : public MediaEngineSource {
 public:
  explicit MediaEngineFakeVideoSource(
      const MediaEngineSourceInitialParams& aParams);

  static nsString GetGroupId();

  AbstractCanonical<MediaEngineSourceSettings>* CanonicalSettings() override {
    return &mSettings;
  }
  const MediaEngineSourceSettings& Settings() const override {
    return mSettings.Ref();
  }
  AbstractCanonical<MediaEngineSourceCapabilities>* CanonicalCapabilities()
      override {
    return &mCapabilities;
  }
  const MediaEngineSourceCapabilities& Capabilities() const override {
    return mCapabilities.Ref();
  }

  nsresult Allocate(const dom::MediaTrackConstraints& aConstraints,
                    const MediaEnginePrefs& aPrefs, uint64_t aWindowID,
                    const char** aOutBadConstraint) override;
  void SetTrack(const RefPtr<MediaTrack>& aTrack,
                const PrincipalHandle& aPrincipal) override;
  nsresult Start() override;
  nsresult Reconfigure(const dom::MediaTrackConstraints& aConstraints,
                       const MediaEnginePrefs& aPrefs,
                       const char** aOutBadConstraint) override;
  nsresult Stop() override;
  nsresult Deallocate() override;

  uint32_t GetBestFitnessDistance(
      const nsTArray<const NormalizedConstraintSet*>& aConstraintSets,
      const MediaEnginePrefs& aPrefs) const override;

  bool IsFake() const override { return true; }

 protected:
  ~MediaEngineFakeVideoSource() {
    mGeneratedImageListener.DisconnectIfExists();
  }

  static MediaEngineSourceSettings DefaultSettings();
  static MediaEngineSourceCapabilities MakeCapabilities(
      bool aResizeModeEnabled);

  void OnGeneratedImage(RefPtr<layers::Image> aImage, TimeStamp aTime,
                        VideoRotation aRotation);

  // Owning thread only.
  RefPtr<FakeVideoSource> mCapturer;
  MediaEventListener mGeneratedImageListener;

  // Current state of this source.
  MediaEngineSourceState mState = kReleased;
  RefPtr<SourceMediaTrack> mTrack;
  PrincipalHandle mPrincipalHandle = PRINCIPAL_HANDLE_NONE;

  MediaEnginePrefs mOpts;

  // Owning thread only.
  Canonical<MediaEngineSourceSettings> mSettings;
  Canonical<MediaEngineSourceCapabilities> mCapabilities;
};

MediaEngineFakeVideoSource::MediaEngineFakeVideoSource(
    const MediaEngineSourceInitialParams& aParams)
    : mSettings(MediaManager::MediaThread(),
                aParams.mSettings.valueOr(DefaultSettings()),
                "MediaEngineFakeVideoSource::mSettings"),
      mCapabilities(MediaManager::MediaThread(),
                    aParams.mCapabilities.valueOr(MakeCapabilities(
                        MediaEnginePrefs().mResizeModeEnabled)),
                    "MediaEngineFakeVideoSource::mCapabilities") {}

/* static */
MediaEngineSourceSettings MediaEngineFakeVideoSource::DefaultSettings() {
  MediaEngineSourceSettings settings;
  settings.mWidth = Some(int32_t(VIDEO_WIDTH_DEFAULT));
  settings.mHeight = Some(int32_t(VIDEO_HEIGHT_DEFAULT));
  settings.mFrameRate = Some(double(MediaEnginePrefs::DEFAULT_VIDEO_FPS));
  settings.mFacingMode = Some(nsString(NS_ConvertASCIItoUTF16(
      dom::GetEnumString(VideoFacingModeEnum::Environment))));
  settings.mResizeMode = Some(nsString(NS_ConvertASCIItoUTF16(
      dom::GetEnumString(dom::VideoResizeModeEnum::None))));
  return settings;
}

nsString MediaEngineFakeVideoSource::GetGroupId() {
  return u"Fake Video Group"_ns;
}

uint32_t MediaEngineFakeVideoSource::GetBestFitnessDistance(
    const nsTArray<const NormalizedConstraintSet*>& aConstraintSets,
    const MediaEnginePrefs& aPrefs) const {
  AssertIsOnOwningThread();

  uint64_t distance = 0;

#ifdef MOZ_WEBRTC
  // distance is read from first entry only
  if (aConstraintSets.Length() >= 1) {
    const auto* cs = aConstraintSets.ElementAt(0);
    const auto resizeMode = MediaConstraintsHelper::GetResizeMode(*cs, aPrefs);
    using H = MediaConstraintsHelper;
    Maybe<nsString> facingMode = Nothing();
    distance += H::FitnessDistance(facingMode, cs->mFacingMode);

    if (resizeMode.valueOr(dom::VideoResizeModeEnum::None) ==
        dom::VideoResizeModeEnum::None) {
      distance +=
          H::FitnessDistance(VIDEO_WIDTH_DEFAULT, cs->mWidth) +
          H::FitnessDistance(VIDEO_HEIGHT_DEFAULT, cs->mHeight) +
          H::FitnessDistance(AssertedCast<double>(aPrefs.mFPS), cs->mFrameRate);
    } else {
      distance += H::FeasibilityDistance(VIDEO_WIDTH_DEFAULT, cs->mWidth) +
                  H::FeasibilityDistance(VIDEO_HEIGHT_DEFAULT, cs->mHeight) +
                  H::FeasibilityDistance(AssertedCast<double>(aPrefs.mFPS),
                                         cs->mFrameRate);
    }
  }
#endif

  return SaturatingCast<uint32_t>(distance);
}

/* static */
MediaEngineSourceCapabilities MediaEngineFakeVideoSource::MakeCapabilities(
    bool aResizeModeEnabled) {
  MediaEngineSourceCapabilities capabilities;
  capabilities.mFacingMode.AppendElement(
      NS_ConvertASCIItoUTF16(GetEnumString(VideoFacingModeEnum::Environment)));
  if (aResizeModeEnabled) {
    capabilities.mResizeMode.AppendElement(
        NS_ConvertASCIItoUTF16(GetEnumString(dom::VideoResizeModeEnum::None)));
    capabilities.mResizeMode.AppendElement(NS_ConvertASCIItoUTF16(
        GetEnumString(dom::VideoResizeModeEnum::Crop_and_scale)));
  }
  capabilities.mWidth =
      Some(MediaEngineSourceCapabilities::ULongRange{1, VIDEO_WIDTH_MAX});
  capabilities.mHeight =
      Some(MediaEngineSourceCapabilities::ULongRange{1, VIDEO_HEIGHT_MAX});
  capabilities.mFrameRate = Some(MediaEngineSourceCapabilities::DoubleRange{
      0, double(MediaEnginePrefs::DEFAULT_VIDEO_FPS)});
  return capabilities;
}

nsresult MediaEngineFakeVideoSource::Allocate(
    const MediaTrackConstraints& aConstraints, const MediaEnginePrefs& aPrefs,
    uint64_t aWindowID, const char** aOutBadConstraint) {
  AssertIsOnOwningThread();

  MOZ_ASSERT(mState == kReleased);

  FlattenedConstraints c(aConstraints);

  // emulator debug is very, very slow; reduce load on it with smaller/slower
  // fake video
  mOpts = aPrefs;
  mOpts.mWidth = aPrefs.mWidth ? aPrefs.mWidth : VIDEO_WIDTH_DEFAULT;
  mOpts.mHeight = aPrefs.mHeight ? aPrefs.mHeight : VIDEO_HEIGHT_DEFAULT;

  const auto resizeMode = MediaConstraintsHelper::GetResizeMode(c, mOpts);
  const auto resizeModeString = resizeMode.map(
      [](auto aRM) { return NS_ConvertASCIItoUTF16(dom::GetEnumString(aRM)); });
  if (resizeMode.valueOr(VideoResizeModeEnum::None) ==
      VideoResizeModeEnum::Crop_and_scale) {
    mOpts.mWidth = c.mWidth.Get(mOpts.mWidth);
    mOpts.mHeight = c.mHeight.Get(mOpts.mHeight);
    mOpts.mFPS = std::min(mOpts.mFPS, SaturatingCast<int32_t>(c.mFrameRate.Get(
                                          AssertedCast<double>(mOpts.mFPS))));
  }

  mOpts.mWidth = std::clamp(mOpts.mWidth, 1, VIDEO_WIDTH_MAX);
  mOpts.mHeight = std::clamp(mOpts.mHeight, 1, VIDEO_HEIGHT_MAX);
  mOpts.mFPS = std::clamp(mOpts.mFPS, 0, MediaEnginePrefs::DEFAULT_VIDEO_FPS);

  nsCOMPtr<nsISerialEventTarget> target = GetCurrentSerialEventTarget();
  mCapturer = MakeRefPtr<FakeVideoSource>(target);
  mGeneratedImageListener = mCapturer->GeneratedImageEvent().Connect(
      target, this, &MediaEngineFakeVideoSource::OnGeneratedImage);

  MediaEngineSourceSettings settings = mSettings.Ref();
  settings.mFrameRate = Some(double(mOpts.mFPS));
  settings.mWidth = Some(mOpts.mWidth);
  settings.mHeight = Some(mOpts.mHeight);
  settings.mResizeMode =
      resizeModeString.map([](const auto& aStr) { return nsString(aStr); });
  mSettings = settings;
  mCapabilities = MakeCapabilities(mOpts.mResizeModeEnabled);

  mState = kAllocated;
  return NS_OK;
}

nsresult MediaEngineFakeVideoSource::Deallocate() {
  AssertIsOnOwningThread();

  MOZ_ASSERT(mState == kStopped || mState == kAllocated);

  mGeneratedImageListener.Disconnect();
  mCapturer = nullptr;
  if (mTrack) {
    mTrack->End();
    mTrack = nullptr;
    mPrincipalHandle = PRINCIPAL_HANDLE_NONE;
  }
  mState = kReleased;

  return NS_OK;
}

void MediaEngineFakeVideoSource::SetTrack(const RefPtr<MediaTrack>& aTrack,
                                          const PrincipalHandle& aPrincipal) {
  AssertIsOnOwningThread();

  MOZ_ASSERT(mState == kAllocated);
  MOZ_ASSERT(!mTrack);
  MOZ_ASSERT(aTrack->AsSourceTrack());

  mTrack = aTrack->AsSourceTrack();
  mPrincipalHandle = aPrincipal;
}

nsresult MediaEngineFakeVideoSource::Start() {
  AssertIsOnOwningThread();

  MOZ_ASSERT(mState == kAllocated || mState == kStopped);
  MOZ_ASSERT(mTrack, "SetTrack() must happen before Start()");

  int32_t rv = mCapturer->StartCapture(
      mOpts.mWidth, mOpts.mHeight, TimeDuration::FromSeconds(1.0 / mOpts.mFPS));
  if (NS_WARN_IF(rv != 0)) {
    return NS_ERROR_FAILURE;
  }

  mState = kStarted;
  return NS_OK;
}

nsresult MediaEngineFakeVideoSource::Stop() {
  AssertIsOnOwningThread();

  if (mState == kStopped || mState == kAllocated) {
    return NS_OK;
  }

  MOZ_ASSERT(mState == kStarted);
  MOZ_ASSERT(mTrack);

  int32_t rv = mCapturer->StopCapture();
  if (NS_WARN_IF(rv != 0)) {
    return NS_ERROR_FAILURE;
  }

  mState = kStopped;

  return NS_OK;
}

nsresult MediaEngineFakeVideoSource::Reconfigure(
    const MediaTrackConstraints& aConstraints, const MediaEnginePrefs& aPrefs,
    const char** aOutBadConstraint) {
  return NS_OK;
}

void MediaEngineFakeVideoSource::OnGeneratedImage(RefPtr<layers::Image> aImage,
                                                  TimeStamp aTime,
                                                  VideoRotation aRotation) {
  VideoSegment segment;
  segment.AppendFrame(
      aImage.forget(), gfx::IntSize(mOpts.mWidth, mOpts.mHeight),
      mPrincipalHandle, /*aForceBlack=*/false, aTime,
      media::TimeUnit::Invalid(), media::TimeUnit::Invalid(), aRotation);
  mTrack->AppendData(&segment);
}

// This class is created on the media thread, as part of Start(), then entirely
// self-sustained until destruction, just forwarding calls to Pull().
class AudioSourcePullListener : public MediaTrackListener {
 public:
  AudioSourcePullListener(RefPtr<SourceMediaTrack> aTrack,
                          const PrincipalHandle& aPrincipalHandle,
                          uint32_t aFrequency)
      : mTrack(std::move(aTrack)),
        mPrincipalHandle(aPrincipalHandle),
        mSineGenerator(MakeUnique<SineWaveGenerator<int16_t>>(
            mTrack->mSampleRate, aFrequency)) {
    MOZ_COUNT_CTOR(AudioSourcePullListener);
  }

  MOZ_COUNTED_DTOR(AudioSourcePullListener)

  void NotifyPull(MediaTrackGraph* aGraph, TrackTime aEndOfAppendedData,
                  TrackTime aDesiredTime) override;

  const RefPtr<SourceMediaTrack> mTrack;
  const PrincipalHandle mPrincipalHandle;
  const UniquePtr<SineWaveGenerator<int16_t>> mSineGenerator;
};

/**
 * Fake audio source.
 */
class MediaEngineFakeAudioSource : public MediaEngineSource {
 public:
  MediaEngineFakeAudioSource();

  static nsString GetUUID();
  static nsString GetGroupId();

  nsresult Allocate(const dom::MediaTrackConstraints& aConstraints,
                    const MediaEnginePrefs& aPrefs, uint64_t aWindowID,
                    const char** aOutBadConstraint) override;
  void SetTrack(const RefPtr<MediaTrack>& aTrack,
                const PrincipalHandle& aPrincipal) override;
  nsresult Start() override;
  nsresult Reconfigure(const dom::MediaTrackConstraints& aConstraints,
                       const MediaEnginePrefs& aPrefs,
                       const char** aOutBadConstraint) override;
  nsresult Stop() override;
  nsresult Deallocate() override;

  bool IsFake() const override { return true; }

 protected:
  ~MediaEngineFakeAudioSource() = default;

  static MediaEngineSourceSettings InitialSettings();
  static MediaEngineSourceCapabilities MakeCapabilities();

  // Current state of this source.
  MediaEngineSourceState mState = kReleased;
  RefPtr<SourceMediaTrack> mTrack;
  PrincipalHandle mPrincipalHandle = PRINCIPAL_HANDLE_NONE;
  uint32_t mFrequency = 1000;
  RefPtr<AudioSourcePullListener> mPullListener;

  // Owning thread only.
  Canonical<MediaEngineSourceSettings> mSettings;
  Canonical<MediaEngineSourceCapabilities> mCapabilities;

 public:
  AbstractCanonical<MediaEngineSourceSettings>* CanonicalSettings() override {
    return &mSettings;
  }
  const MediaEngineSourceSettings& Settings() const override {
    return mSettings.Ref();
  }
  AbstractCanonical<MediaEngineSourceCapabilities>* CanonicalCapabilities()
      override {
    return &mCapabilities;
  }
  const MediaEngineSourceCapabilities& Capabilities() const override {
    return mCapabilities.Ref();
  }
};

nsString MediaEngineFakeAudioSource::GetUUID() {
  return u"B7CBD7C1-53EF-42F9-8353-73F61C70C092"_ns;
}

nsString MediaEngineFakeAudioSource::GetGroupId() {
  return u"Fake Audio Group"_ns;
}

MediaEngineFakeAudioSource::MediaEngineFakeAudioSource()
    : mSettings(MediaManager::MediaThread(), InitialSettings(),
                "MediaEngineFakeAudioSource::mSettings"),
      mCapabilities(MediaManager::MediaThread(), MakeCapabilities(),
                    "MediaEngineFakeAudioSource::mCapabilities") {}

/* static */
MediaEngineSourceSettings MediaEngineFakeAudioSource::InitialSettings() {
  MediaEngineSourceSettings settings;
  settings.mAutoGainControl = Some(false);
  settings.mEchoCancellation = Some(false);
  settings.mNoiseSuppression = Some(false);
  settings.mChannelCount = Some(1);
  return settings;
}

/* static */
MediaEngineSourceCapabilities MediaEngineFakeAudioSource::MakeCapabilities() {
  MediaEngineSourceCapabilities capabilities;
  capabilities.mEchoCancellation.AppendElement(false);
  capabilities.mAutoGainControl.AppendElement(false);
  capabilities.mNoiseSuppression.AppendElement(false);
  capabilities.mChannelCount =
      Some(MediaEngineSourceCapabilities::ULongRange{1, 1});
  return capabilities;
}

nsresult MediaEngineFakeAudioSource::Allocate(
    const MediaTrackConstraints& aConstraints, const MediaEnginePrefs& aPrefs,
    uint64_t aWindowID, const char** aOutBadConstraint) {
  AssertIsOnOwningThread();

  MOZ_ASSERT(mState == kReleased);

  mFrequency = aPrefs.mFreq ? aPrefs.mFreq : 1000;

  mState = kAllocated;
  return NS_OK;
}

nsresult MediaEngineFakeAudioSource::Deallocate() {
  AssertIsOnOwningThread();

  MOZ_ASSERT(mState == kStopped || mState == kAllocated);

  if (mTrack) {
    mTrack->End();
    mTrack = nullptr;
    mPrincipalHandle = PRINCIPAL_HANDLE_NONE;
  }
  mState = kReleased;
  return NS_OK;
}

void MediaEngineFakeAudioSource::SetTrack(const RefPtr<MediaTrack>& aTrack,
                                          const PrincipalHandle& aPrincipal) {
  AssertIsOnOwningThread();

  MOZ_ASSERT(mState == kAllocated);
  MOZ_ASSERT(!mTrack);
  MOZ_ASSERT(aTrack->AsSourceTrack());

  mTrack = aTrack->AsSourceTrack();
  mPrincipalHandle = aPrincipal;
}

nsresult MediaEngineFakeAudioSource::Start() {
  AssertIsOnOwningThread();

  if (mState == kStarted) {
    return NS_OK;
  }

  MOZ_ASSERT(mState == kAllocated || mState == kStopped);
  MOZ_ASSERT(mTrack, "SetTrack() must happen before Start()");

  if (!mPullListener) {
    mPullListener = MakeAndAddRef<AudioSourcePullListener>(
        mTrack, mPrincipalHandle, mFrequency);
  }

  mState = kStarted;

  NS_DispatchToMainThread(NS_NewRunnableFunction(
      __func__, [track = mTrack, listener = mPullListener]() {
        if (track->IsDestroyed()) {
          return;
        }
        track->AddListener(listener);
        track->SetPullingEnabled(true);
      }));

  return NS_OK;
}

nsresult MediaEngineFakeAudioSource::Stop() {
  AssertIsOnOwningThread();

  if (mState == kStopped || mState == kAllocated) {
    return NS_OK;
  }
  MOZ_ASSERT(mState == kStarted);
  mState = kStopped;

  NS_DispatchToMainThread(NS_NewRunnableFunction(
      __func__, [track = mTrack, listener = std::move(mPullListener)]() {
        if (track->IsDestroyed()) {
          return;
        }
        track->RemoveListener(listener);
        track->SetPullingEnabled(false);
      }));
  return NS_OK;
}

nsresult MediaEngineFakeAudioSource::Reconfigure(
    const MediaTrackConstraints& aConstraints, const MediaEnginePrefs& aPrefs,
    const char** aOutBadConstraint) {
  return NS_OK;
}

void AudioSourcePullListener::NotifyPull(MediaTrackGraph* aGraph,
                                         TrackTime aEndOfAppendedData,
                                         TrackTime aDesiredTime) {
  TRACE_COMMENT("SourceMediaTrack::NotifyPull", "SourceMediaTrack %p",
                mTrack.get());
  AudioSegment segment;
  TrackTicks delta = aDesiredTime - aEndOfAppendedData;
  CheckedInt<size_t> bufferSize(sizeof(int16_t));
  bufferSize *= delta;
  RefPtr<SharedBuffer> buffer = SharedBuffer::Create(bufferSize);
  int16_t* dest = static_cast<int16_t*>(buffer->Data());
  mSineGenerator->generate(dest, delta);
  AutoTArray<const int16_t*, 1> channels;
  channels.AppendElement(dest);
  segment.AppendFrames(buffer.forget(), channels, delta, mPrincipalHandle);
  mTrack->AppendData(&segment);
}

MediaEngineFake::MediaEngineFake() = default;
MediaEngineFake::~MediaEngineFake() = default;

void MediaEngineFake::EnumerateDevices(
    MediaSourceEnum aMediaSource, MediaSinkEnum aMediaSink,
    nsTArray<RefPtr<MediaDevice>>* aDevices) {
  AssertIsOnOwningThread();
  using IsScary = MediaDevice::IsScary;
  using OsPromptable = MediaDevice::OsPromptable;

  if (aMediaSink == MediaSinkEnum::Speaker) {
    NS_WARNING("No default implementation for MediaSinkEnum::Speaker");
  }

  switch (aMediaSource) {
    case MediaSourceEnum::Camera: {
      nsString name = FakeVideoName();
      aDevices->EmplaceBack(
          new MediaDevice(this, aMediaSource, name, /*aRawId=*/name,
                          MediaEngineFakeVideoSource::GetGroupId(), IsScary::No,
                          OsPromptable::No));
      return;
    }
    case MediaSourceEnum::Microphone:
      aDevices->EmplaceBack(
          new MediaDevice(this, aMediaSource, u"Default Audio Device"_ns,
                          MediaEngineFakeAudioSource::GetUUID(),
                          MediaEngineFakeAudioSource::GetGroupId(), IsScary::No,
                          OsPromptable::No));
      return;
    default:
      MOZ_ASSERT_UNREACHABLE("Unsupported source type");
      return;
  }
}

RefPtr<MediaEngineSource> MediaEngineFake::CreateSource(
    const MediaDevice* aMediaDevice) {
  return CreateSourceFrom(nullptr, aMediaDevice,
                          MediaEngineSourceInitialParams());
}

RefPtr<MediaEngineSource> MediaEngineFake::CreateSourceFrom(
    const MediaEngineSource* aSource, const MediaDevice* aMediaDevice,
    const MediaEngineSourceInitialParams& aParams) {
  MOZ_ASSERT(aMediaDevice->mEngine == this);
  switch (aMediaDevice->mMediaSource) {
    case MediaSourceEnum::Camera:
      return MakeRefPtr<MediaEngineFakeVideoSource>(aParams);
    case MediaSourceEnum::Microphone:
      // Settings and capabilities are constant.
      return MakeRefPtr<MediaEngineFakeAudioSource>();
    default:
      MOZ_ASSERT_UNREACHABLE("Unsupported source type");
      return nullptr;
  }
}

}  // namespace mozilla
