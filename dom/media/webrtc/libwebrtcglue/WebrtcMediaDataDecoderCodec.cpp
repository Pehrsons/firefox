/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebrtcMediaDataDecoderCodec.h"

#include <optional>

#include "ImageContainer.h"
#include "MediaDataCodec.h"
#include "MediaDataDecoderProxy.h"
#include "PDMFactory.h"
#include "PDMFactorySupport.h"
#include "VideoUtils.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/layers/ImageBridgeChild.h"
#include "mozilla/media/MediaUtils.h"
#include "nsThreadUtils.h"
// #include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "modules/video_coding/utility/vp8_header_parser.h"
#include "modules/video_coding/utility/vp9_uncompressed_header_parser.h"

namespace mozilla {

/* static */
bool WebrtcMediaDataDecoder::IsCodecEnabled(webrtc::VideoCodecType aCodec) {
  switch (aCodec) {
    case webrtc::VideoCodecType::kVideoCodecVP8:
    case webrtc::VideoCodecType::kVideoCodecVP9:
      return StaticPrefs::media_navigator_mediadatadecoder_vpx_enabled();
    case webrtc::VideoCodecType::kVideoCodecH264:
      return StaticPrefs::media_navigator_mediadatadecoder_h264_enabled();
    case webrtc::VideoCodecType::kVideoCodecAV1:
      return StaticPrefs::media_navigator_mediadatadecoder_av1_enabled();
    case webrtc::VideoCodecType::kVideoCodecGeneric:
    case webrtc::VideoCodecType::kVideoCodecH265:
      return false;
  }
  return false;
}

/* static */
CreateDecoderParams::OptionSet WebrtcMediaDataDecoder::WebrtcDecoderOptions() {
  return CreateDecoderParams::OptionSet(
      CreateDecoderParams::Option::LowLatency,
      CreateDecoderParams::Option::FullH264Parsing,
      CreateDecoderParams::Option::ErrorIfNoInitializationData);
}

/* static */
RefPtr<PlatformDecoderModule::SupportsDecoderPromise>
WebrtcMediaDataDecoder::Supports(webrtc::VideoCodecType aCodecType,
                                 SupportDecoderParams aParams) {
  if (!IsCodecEnabled(aCodecType)) {
    return PlatformDecoderModule::SupportsDecoderPromise::CreateAndResolve(
        media::DecodeSupportSet{}, __func__);
  }
  aParams.mOptions = WebrtcDecoderOptions();
  return PDMFactorySupport::IsSupportedAsync(aParams)->Map(
      GetCurrentSerialEventTarget(), __func__,
      AdjustWebrtcDecodeSupportFunctionForCodec(ToCodecType(aCodecType)));
}

WebrtcMediaDataDecoder::WebrtcMediaDataDecoder(nsACString& aCodecMimeType,
                                               TrackingId aTrackingId)
    : mThreadPool(GetMediaThreadPool(MediaThreadType::SUPERVISOR)),
      mTaskQueue(TaskQueue::Create(do_AddRef(mThreadPool),
                                   "WebrtcMediaDataDecoder::mTaskQueue")),
      mImageContainer(MakeAndAddRef<layers::ImageContainer>(
          layers::ImageUsageType::Webrtc,
          layers::ImageContainer::ASYNCHRONOUS)),
      mFactory(new PDMFactory()),
      mTrackType(TrackInfo::kUndefinedTrack),
      mCodecType(aCodecMimeType),
      mTrackingId(std::move(aTrackingId)) {}

WebrtcMediaDataDecoder::~WebrtcMediaDataDecoder() = default;

bool WebrtcMediaDataDecoder::Configure(
    const webrtc::VideoDecoder::Settings& settings) {
  nsCString codec;
  mTrackType = TrackInfo::kVideoTrack;
  mInfo = VideoInfo(settings.max_render_resolution().Width(),
                    settings.max_render_resolution().Height());
  mInfo.mMimeType = mCodecType;

#ifdef MOZ_WIDGET_GTK
  if (mInfo.mMimeType.EqualsLiteral("video/vp8") &&
      !StaticPrefs::media_navigator_mediadatadecoder_vp8_hardware_enabled()) {
    mDisabledHardwareAcceleration = true;
  }
#endif

  return WEBRTC_VIDEO_CODEC_OK == CreateDecoder();
}

std::optional<uint8_t> WebrtcMediaDataDecoder::ExtractQp(
    const webrtc::EncodedImage& aImage) {
  // Platform decoders don't expose the QP, so parse it from the bitstream for
  // the inbound-rtp qpSum stat. Bug 2049503 will expose it from PEMs/PDMs.
  int qp = -1;
  if (mInfo.mMimeType.EqualsLiteral("video/vp8")) {
    if (webrtc::vp8::GetQp(aImage.data(), aImage.size(), &qp)) {
      return static_cast<uint8_t>(qp);
    }
  } else if (mInfo.mMimeType.EqualsLiteral("video/vp9")) {
    if (webrtc::vp9::GetQp(aImage.data(), aImage.size(), &qp)) {
      return static_cast<uint8_t>(qp);
    }
  }
  return std::nullopt;
}

// Returns the sizes of the frames in aImage. libwebrtc concatenates the spatial
// layers of a VP9 SVC frame, each a shown frame, into one buffer without a
// superframe index. These are split into one sample per layer, like ffmpeg's
// vp9_superframe_split bitstream filter does with an indexed superframe, as
// decoders taking a single frame per sample decode only the first frame, and
// VideoToolbox rejects superframes with several shown frames.
static AutoTArray<size_t, 3> FrameSizes(const webrtc::EncodedImage& aImage,
                                        bool aIsVp9) {
  AutoTArray<size_t, 3> sizes;
  if (aIsVp9) {
    size_t totalSize = 0;
    for (int i = 0; i <= aImage.SpatialIndex().value_or(0); ++i) {
      if (std::optional<size_t> size = aImage.SpatialLayerFrameSize(i)) {
        sizes.AppendElement(*size);
        totalSize += *size;
      }
    }
    if (totalSize == aImage.size()) {
      return sizes;
    }
    sizes.Clear();
  }
  sizes.AppendElement(aImage.size());
  return sizes;
}

int32_t WebrtcMediaDataDecoder::Decode(const webrtc::EncodedImage& aInputImage,
                                       bool aMissingFrames,
                                       int64_t aRenderTimeMs) {
  if (!mCallback || !mDecoder) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  if (!aInputImage.data() || !aInputImage.size()) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  // Always start with a complete key frame.
  if (mNeedKeyframe) {
    if (aInputImage._frameType != webrtc::VideoFrameType::kVideoFrameKey)
      return WEBRTC_VIDEO_CODEC_ERROR;
    // We have a key frame - is it complete?
    mNeedKeyframe = false;
  }

  auto disabledHardwareAcceleration =
      MakeScopeExit([&] { mDisabledHardwareAcceleration = true; });

  const AutoTArray<size_t, 3> frameSizes =
      FrameSizes(aInputImage, mInfo.mMimeType.EqualsLiteral("video/vp9"));
  AutoTArray<RefPtr<MediaRawData>, 3> compressedFrames;
  size_t offset = 0;
  for (size_t size : frameSizes) {
    RefPtr compressedFrame =
        MakeRefPtr<MediaRawData>(aInputImage.data() + offset, size);
    if (!compressedFrame->Data()) {
      return WEBRTC_VIDEO_CODEC_MEMORY;
    }
    compressedFrame->mTime =
        media::TimeUnit::FromMicroseconds(aInputImage.RtpTimestamp());
    compressedFrame->mTimecode =
        media::TimeUnit::FromMicroseconds(aRenderTimeMs * 1000);
    // Only the lowest spatial layer of an SVC keyframe is a keyframe.
    compressedFrame->mKeyframe =
        offset == 0 &&
        aInputImage._frameType == webrtc::VideoFrameType::kVideoFrameKey;
    compressedFrames.AppendElement(std::move(compressedFrame));
    offset += size;
  }
  {
    for (const RefPtr<MediaRawData>& compressedFrame : compressedFrames) {
      media::Await(
          do_AddRef(mThreadPool), mDecoder->Decode(compressedFrame),
          [&](const MediaDataDecoder::DecodedData& aResults) {
            mResults.AppendElements(aResults);
            mError = NS_OK;
          },
          [&](const MediaResult& aError) { mError = aError; });
      if (NS_FAILED(mError)) {
        break;
      }
    }

    const std::optional<uint8_t> qp = ExtractQp(aInputImage);
    for (size_t i = 0; i < mResults.Length(); ++i) {
      const RefPtr<MediaData>& frame = mResults[i];
      // There is one picture per spatial layer of a VP9 SVC frame. Only the
      // last one, the highest layer, is to be shown.
      if (i + 1 < mResults.Length() && mResults[i + 1]->mTime == frame->mTime) {
        continue;
      }
      MOZ_ASSERT(frame->mType == MediaData::Type::VIDEO_DATA);
      RefPtr<VideoData> video = frame->As<VideoData>();
      MOZ_ASSERT(video);
      if (!video->mImage) {
        // Nothing to display.
        continue;
      }
      webrtc::scoped_refptr<ImageBuffer> image(
          new webrtc::RefCountedObject<ImageBuffer>(std::move(video->mImage)));

      auto videoFrame = webrtc::VideoFrame::Builder()
                            .set_video_frame_buffer(image)
                            .set_timestamp_rtp(aInputImage.RtpTimestamp())
                            .set_rotation(aInputImage.rotation_)
                            .build();
      mCallback->Decoded(videoFrame, /* decode_time_ms */ std::nullopt, qp);
    }
    mResults.Clear();
  }

  if (NS_FAILED(mError) && mError != NS_ERROR_DOM_MEDIA_CANCELED) {
    CreateDecoder();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (NS_FAILED(mError)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  disabledHardwareAcceleration.release();
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t WebrtcMediaDataDecoder::RegisterDecodeCompleteCallback(
    webrtc::DecodedImageCallback* aCallback) {
  mCallback = aCallback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t WebrtcMediaDataDecoder::Release() {
  if (mDecoder) {
    RefPtr<MediaDataDecoder> decoder = std::move(mDecoder);
    decoder->Flush()->Then(mTaskQueue, __func__,
                           [decoder]() { decoder->Shutdown(); });
  }

  mNeedKeyframe = true;
  mError = NS_OK;

  return WEBRTC_VIDEO_CODEC_OK;
}

bool WebrtcMediaDataDecoder::OnTaskQueue() const {
  return mTaskQueue->IsOnCurrentThread();
}

int32_t WebrtcMediaDataDecoder::CreateDecoder() {
  RefPtr<layers::KnowsCompositor> knowsCompositor =
      layers::ImageBridgeChild::GetSingleton();

  if (mDecoder) {
    Release();
  }

  RefPtr<TaskQueue> tq =
      TaskQueue::Create(GetMediaThreadPool(MediaThreadType::PLATFORM_DECODER),
                        "webrtc decode TaskQueue");
  auto options = WebrtcDecoderOptions();
  if (mDisabledHardwareAcceleration) {
    options += CreateDecoderParams::Option::HardwareDecoderNotAllowed;
  }
  RefPtr<MediaDataDecoder> decoder;

  media::Await(
      do_AddRef(mThreadPool), InvokeAsync(tq, __func__, [&] {
        RefPtr<GenericPromise> p =
            mFactory
                ->CreateDecoder({mInfo, options, mTrackType, mImageContainer,
                                 knowsCompositor, Some(mTrackingId)})
                ->Then(
                    tq, __func__,
                    [&](RefPtr<MediaDataDecoder>&& aDecoder) {
                      decoder = std::move(aDecoder);
                      return GenericPromise::CreateAndResolve(true, __func__);
                    },
                    [](const MediaResult& aResult) {
                      return GenericPromise::CreateAndReject(
                          NS_ERROR_DOM_MEDIA_FATAL_ERR, __func__);
                    });
        return p;
      }));

  if (!decoder) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // We need to wrap our decoder in a MediaDataDecoderProxy so that it always
  // run on an nsISerialEventTarget (which the webrtc code doesn't do)
  mDecoder = new MediaDataDecoderProxy(decoder.forget(), tq.forget());

  media::Await(
      do_AddRef(mThreadPool), mDecoder->Init(),
      [&](TrackInfo::TrackType) { mError = NS_OK; },
      [&](const MediaResult& aError) { mError = aError; });

  return NS_SUCCEEDED(mError) ? WEBRTC_VIDEO_CODEC_OK
                              : WEBRTC_VIDEO_CODEC_ERROR;
}

}  // namespace mozilla
