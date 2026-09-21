/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaEngineSource.h"

#include "mozilla/dom/MediaTrackCapabilitiesBinding.h"
#include "mozilla/dom/MediaTrackSettingsBinding.h"

namespace mozilla {

using dom::MediaSourceEnum;
using dom::MediaTrackSettings;

// These need a definition somewhere because template
// code is allowed to take their address, and they aren't
// guaranteed to have one without this.
const unsigned int MediaEngineSource::kMaxDeviceNameLength;
const unsigned int MediaEngineSource::kMaxUniqueIdLength;

/* static */
bool MediaEngineSource::IsVideo(MediaSourceEnum aSource) {
  switch (aSource) {
    case MediaSourceEnum::Camera:
    case MediaSourceEnum::Screen:
    case MediaSourceEnum::Window:
    case MediaSourceEnum::Browser:
      return true;
    case MediaSourceEnum::Microphone:
    case MediaSourceEnum::AudioCapture:
    case MediaSourceEnum::Other:
      return false;
    default:
      MOZ_ASSERT_UNREACHABLE("Unknown type");
      return false;
  }
}

/* static */
bool MediaEngineSource::IsAudio(MediaSourceEnum aSource) {
  switch (aSource) {
    case MediaSourceEnum::Microphone:
    case MediaSourceEnum::AudioCapture:
      return true;
    case MediaSourceEnum::Camera:
    case MediaSourceEnum::Screen:
    case MediaSourceEnum::Window:
    case MediaSourceEnum::Browser:
    case MediaSourceEnum::Other:
      return false;
    default:
      MOZ_ASSERT_UNREACHABLE("Unknown type");
      return false;
  }
}

bool MediaEngineSource::IsFake() const { return false; }

nsresult MediaEngineSource::FocusOnSelectedSource() {
  return NS_ERROR_NOT_AVAILABLE;
}

nsresult MediaEngineSource::TakePhoto(MediaEnginePhotoCallback* aCallback) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

MediaEngineSource::~MediaEngineSource() = default;

dom::MediaTrackSettings MediaEngineSourceSettings::ToMediaTrackSettings()
    const {
  dom::MediaTrackSettings settings;
  auto construct = [](auto& aOptional, const auto& aMaybe) {
    if (aMaybe) {
      aOptional.Construct(*aMaybe);
    }
  };
  construct(settings.mWidth, mWidth);
  construct(settings.mHeight, mHeight);
  construct(settings.mFrameRate, mFrameRate);
  construct(settings.mFacingMode, mFacingMode);
  construct(settings.mResizeMode, mResizeMode);
  construct(settings.mEchoCancellation, mEchoCancellation);
  construct(settings.mAutoGainControl, mAutoGainControl);
  construct(settings.mNoiseSuppression, mNoiseSuppression);
  construct(settings.mChannelCount, mChannelCount);
  return settings;
}

dom::MediaTrackCapabilities
MediaEngineSourceCapabilities::ToMediaTrackCapabilities() const {
  dom::MediaTrackCapabilities capabilities;
  auto construct = [](auto& aOptional, const auto& aRange) {
    auto& range = aOptional.Construct();
    range.mMin.Construct(aRange.mMin);
    range.mMax.Construct(aRange.mMax);
  };
  if (mWidth) {
    construct(capabilities.mWidth, *mWidth);
  }
  if (mHeight) {
    construct(capabilities.mHeight, *mHeight);
  }
  if (mFrameRate) {
    construct(capabilities.mFrameRate, *mFrameRate);
  }
  if (!mFacingMode.IsEmpty()) {
    capabilities.mFacingMode.Construct(mFacingMode.Clone());
  }
  if (!mResizeMode.IsEmpty()) {
    capabilities.mResizeMode.Construct(mResizeMode.Clone());
  }
  if (!mEchoCancellation.IsEmpty()) {
    capabilities.mEchoCancellation.Construct(mEchoCancellation.Clone());
  }
  if (!mAutoGainControl.IsEmpty()) {
    capabilities.mAutoGainControl.Construct(mAutoGainControl.Clone());
  }
  if (!mNoiseSuppression.IsEmpty()) {
    capabilities.mNoiseSuppression.Construct(mNoiseSuppression.Clone());
  }
  if (mChannelCount) {
    construct(capabilities.mChannelCount, *mChannelCount);
  }
  return capabilities;
}

}  // namespace mozilla
