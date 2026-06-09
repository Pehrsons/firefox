/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebrtcVideoCodecFactory.h"

#include "GmpVideoCodec.h"
#include "MediaDataCodec.h"
#include "MediaMIMETypes.h"
#include "VideoConduit.h"
#include "WebrtcGmpVideoCodec.h"
#include "WebrtcMediaDataDecoderCodec.h"
#include "WebrtcMediaDataEncoderCodec.h"
#include "common/browser_logging/CSFLog.h"
#include "mozilla/StaticPrefs_media.h"
#include "nsThreadUtils.h"

// libwebrtc includes
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "media/engine/simulcast_encoder_adapter.h"
#include "modules/video_coding/codecs/av1/dav1d_decoder.h"
#include "modules/video_coding/codecs/av1/libaom_av1_encoder.h"
#include "modules/video_coding/codecs/vp8/include/vp8.h"
#include "modules/video_coding/codecs/vp9/include/vp9.h"

namespace mozilla {

#define LOGTAG "WebrtcVideoCodecFactory"
#define LOGD(...) CSFLogDebug(LOGTAG, __VA_ARGS__)

// Keep in sync with the doc comment on media.webrtc.encoder_creation_strategy
// in StaticPrefList.yaml.
enum EncoderCreationStrategy {
  PreferWebRTCEncoder = 0,
  PreferPlatformEncoder = 1,
};

// Codecs that libwebrtc can decode in software without a platform decoder.
static media::DecodeSupportSet WebrtcSoftwareDecodeFallback(
    webrtc::VideoCodecType aCodec, const MediaExtendedMIMEType& aMime,
    const SupportDecoderParams& aParams) {
  switch (aCodec) {
    case webrtc::VideoCodecType::kVideoCodecH264:
      return WebrtcGmpDecoderSupports(aMime, aParams);
    case webrtc::VideoCodecType::kVideoCodecVP8:
    case webrtc::VideoCodecType::kVideoCodecVP9:
    case webrtc::VideoCodecType::kVideoCodecAV1:
      return {media::DecodeSupport::SoftwareDecode};
    case webrtc::VideoCodecType::kVideoCodecGeneric:
    case webrtc::VideoCodecType::kVideoCodecH265:
      return {};
  }
  return {};
}

// Resolve aPlatformSupport, and when it reports nothing fall back to
// libwebrtc's built-in software/GMP decode support. Shared by SupportsCodec
// and StrictSupportsCodec, which differ only in how the platform support is
// obtained.
static RefPtr<PlatformDecoderModule::SupportsDecoderPromise>
WithWebrtcDecodeFallback(
    webrtc::VideoCodecType aCodec, const MediaExtendedMIMEType& aMime,
    const SupportDecoderParams& aParams,
    RefPtr<PlatformDecoderModule::SupportsDecoderPromise> aPlatformSupport) {
  // SupportDecoderParams is stack-only and holds a reference to its config, so
  // clone the bits the fallback needs to survive the asynchronous wait.
  MediaExtendedMIMEType mime = aMime;
  UniquePtr<TrackInfo> config = aParams.mConfig.Clone();
  const media::VideoFrameRate rate = aParams.mRate;
  return aPlatformSupport->Then(
      GetCurrentSerialEventTarget(), __func__,
      [aCodec, mime, config = std::move(config),
       rate](media::DecodeSupportSet aSupport)
          -> RefPtr<PlatformDecoderModule::SupportsDecoderPromise> {
        if (!aSupport.isEmpty()) {
          LOGD("WebRTC decode support for %s: platform decoder reports %s",
               mime.OriginalString().get(), fmt::to_string(aSupport).c_str());
          return PlatformDecoderModule::SupportsDecoderPromise::
              CreateAndResolve(aSupport, __func__);
        }
        SupportDecoderParams params{*config, rate};
        const media::DecodeSupportSet fallbackSupport =
            WebrtcSoftwareDecodeFallback(aCodec, mime, params);
        LOGD(
            "WebRTC decode support for %s: no platform decoder, using "
            "libwebrtc built-in fallback (%s)",
            mime.OriginalString().get(),
            fmt::to_string(fallbackSupport).c_str());
        return PlatformDecoderModule::SupportsDecoderPromise::CreateAndResolve(
            fallbackSupport, __func__);
      },
      [](nsresult aRv) {
        LOGD("WebRTC decode support query failed: rv=%s",
             GetStaticErrorName(aRv));
        return PlatformDecoderModule::SupportsDecoderPromise::CreateAndReject(
            aRv, __func__);
      });
}

/* static */
RefPtr<PlatformDecoderModule::SupportsDecoderPromise>
WebrtcVideoDecoderFactory::SupportsCodec(const MediaExtendedMIMEType& aMime,
                                         const SupportDecoderParams& aParams) {
  const auto codec =
      webrtc::PayloadStringToCodecType(std::string(aMime.Subtype().View()));
  LOGD("WebRTC decode support: querying cached codec-support snapshot for %s",
       aMime.OriginalString().get());
  return WithWebrtcDecodeFallback(
      codec, aMime, aParams, WebrtcMediaDataDecoder::Supports(codec, aParams));
}

/* static */
RefPtr<PlatformDecoderModule::SupportsDecoderPromise>
WebrtcVideoDecoderFactory::StrictSupportsCodec(
    const MediaExtendedMIMEType& aMime, const SupportDecoderParams& aParams) {
  const auto codec =
      webrtc::PayloadStringToCodecType(std::string(aMime.Subtype().View()));
  LOGD("WebRTC decode support: probing decoder for %s",
       aMime.OriginalString().get());
  return WithWebrtcDecodeFallback(
      codec, aMime, aParams,
      WebrtcMediaDataDecoder::StrictSupports(codec, aParams));
}

// libwebrtc's built-in software encode support for aConfig, independent of any
// platform encoder.
static media::EncodeSupportSet WebrtcLibwebrtcEncodeSupport(
    const EncoderConfig& aConfig) {
  media::EncodeSupportSet libwebrtcSupport;
  switch (aConfig.mCodec) {
    case CodecType::VP8:
    case CodecType::VP9:
    case CodecType::AV1:
      libwebrtcSupport += media::EncodeSupport::SoftwareEncode;
      break;
    case CodecType::H264:
      libwebrtcSupport += WebrtcGmpEncoderSupports(aConfig);
      break;
    default:
      break;
  }
  return libwebrtcSupport;
}

// Combine libwebrtc's built-in software encode support with the platform
// encoder support produced by aPemSupport, honouring the encoder-creation
// strategy. aPemSupport is invoked lazily (and at most once) so the strict
// path only creates a probe encoder when the platform support is actually
// consulted. Shared by SupportsCodec and StrictSupportsCodec.
template <typename PemSupportFn>
static RefPtr<PlatformEncoderModule::SupportsEncoderPromise>
EncoderSupportsWithPemFn(const EncoderConfig& aConfig,
                         PemSupportFn&& aPemSupport) {
  const auto strategy = static_cast<EncoderCreationStrategy>(
      StaticPrefs::media_webrtc_encoder_creation_strategy());
  const media::EncodeSupportSet libwebrtcSupport =
      WebrtcLibwebrtcEncodeSupport(aConfig);
  switch (strategy) {
    case EncoderCreationStrategy::PreferWebRTCEncoder: {
      // When libwebrtc has SW for this codec, CreateEncoder will always pick
      // it over a PEM, so we report libwebrtc's set alone — any PEM HW
      // capability is intentionally hidden to keep reported support aligned
      // with the encoder that will actually be used.
      if (libwebrtcSupport.isEmpty()) {
        LOGD(
            "WebRTC encode support for %s: prefer-libwebrtc strategy, no "
            "libwebrtc encoder, querying platform encoder support",
            EnumValueToString(aConfig.mCodec));
        return aPemSupport();
      }
      LOGD(
          "WebRTC encode support for %s: prefer-libwebrtc strategy, using "
          "libwebrtc built-in encoder (%s), platform encoder support hidden",
          EnumValueToString(aConfig.mCodec),
          fmt::to_string(libwebrtcSupport).c_str());
      return PlatformEncoderModule::SupportsEncoderPromise::CreateAndResolve(
          libwebrtcSupport, __func__);
    }
    case EncoderCreationStrategy::PreferPlatformEncoder: {
      return aPemSupport()->Then(
          GetCurrentSerialEventTarget(), __func__,
          [libwebrtcSupport,
           codec = aConfig.mCodec](media::EncodeSupportSet aPemSupport) {
            const media::EncodeSupportSet combined =
                aPemSupport + libwebrtcSupport;
            LOGD(
                "WebRTC encode support for %s: prefer-platform strategy, "
                "platform encoder reports %s + libwebrtc %s -> %s",
                EnumValueToString(codec), fmt::to_string(aPemSupport).c_str(),
                fmt::to_string(libwebrtcSupport).c_str(),
                fmt::to_string(combined).c_str());
            return PlatformEncoderModule::SupportsEncoderPromise::
                CreateAndResolve(combined, __func__);
          },
          [codec = aConfig.mCodec](nsresult aRv) {
            LOGD(
                "WebRTC encode support for %s: platform encoder support query "
                "failed: rv=%s",
                EnumValueToString(codec), GetStaticErrorName(aRv));
            return PlatformEncoderModule::SupportsEncoderPromise::
                CreateAndReject(aRv, __func__);
          });
    }
  }
  return PlatformEncoderModule::SupportsEncoderPromise::CreateAndResolve(
      media::EncodeSupportSet{}, __func__);
}

/* static */
RefPtr<PlatformEncoderModule::SupportsEncoderPromise>
WebrtcVideoEncoderFactory::SupportsCodec(const EncoderConfig& aConfig) {
  LOGD("WebRTC encode support: querying cached codec-support snapshot for %s",
       EnumValueToString(aConfig.mCodec));
  return EncoderSupportsWithPemFn(aConfig, [&aConfig]() {
    return MediaDataCodec::SupportsEncoderCodec(aConfig);
  });
}

/* static */
RefPtr<PlatformEncoderModule::SupportsEncoderPromise>
WebrtcVideoEncoderFactory::StrictSupportsCodec(
    const EncoderConfig& aConfig, const RefPtr<TaskQueue>& aTaskQueue) {
  LOGD("WebRTC encode support: probing encoder for %s",
       EnumValueToString(aConfig.mCodec));
  return EncoderSupportsWithPemFn(aConfig, [&aConfig, &aTaskQueue]() {
    return MediaDataCodec::StrictSupportsEncoderCodec(aConfig, aTaskQueue);
  });
}

std::unique_ptr<webrtc::VideoDecoder> WebrtcVideoDecoderFactory::Create(
    const webrtc::Environment& aEnv, const webrtc::SdpVideoFormat& aFormat) {
  std::unique_ptr<webrtc::VideoDecoder> decoder;
  auto type = webrtc::PayloadStringToCodecType(aFormat.name);

  // Attempt to create a decoder using MediaDataDecoder.
  decoder = MediaDataCodec::CreateDecoder(type, mTrackingId);
  if (decoder) {
    LOGD("Created platform MediaDataDecoder for %s", aFormat.name.c_str());
    return decoder;
  }

  LOGD("No platform decoder for %s; falling back to libwebrtc built-in decoder",
       aFormat.name.c_str());
  switch (type) {
    case webrtc::VideoCodecType::kVideoCodecH264: {
      // Get an external decoder
      auto gmpDecoder = GmpVideoCodec::CreateDecoder(mPCHandle, mTrackingId);
      {
        MutexAutoLock lock(mGmpPluginMutex);
        mCreatedGmpPluginEvent.Forward(*gmpDecoder->InitPluginEvent());
        mReleasedGmpPluginEvent.Forward(*gmpDecoder->ReleasePluginEvent());
      }
      decoder = std::move(gmpDecoder);
      break;
    }

    // Use libvpx decoders as fallbacks.
    case webrtc::VideoCodecType::kVideoCodecVP8:
      if (!decoder) {
        decoder = webrtc::CreateVp8Decoder(aEnv);
      }
      break;
    case webrtc::VideoCodecType::kVideoCodecVP9:
      decoder = webrtc::VP9Decoder::Create();
      break;
    case webrtc::VideoCodecType::kVideoCodecAV1:
      decoder = webrtc::CreateDav1dDecoder();
      break;
    default:
      break;
  }

  return decoder;
}

std::unique_ptr<webrtc::VideoEncoder> WebrtcVideoEncoderFactory::Create(
    const webrtc::Environment& aEnv, const webrtc::SdpVideoFormat& aFormat) {
  if (!mInternalFactory->Supports(aFormat)) {
    return nullptr;
  }
  auto type = webrtc::PayloadStringToCodecType(aFormat.name);
  switch (type) {
    case webrtc::VideoCodecType::kVideoCodecGeneric:
    case webrtc::VideoCodecType::kVideoCodecH265:
      MOZ_CRASH("Unimplemented codec");
    case webrtc::VideoCodecType::kVideoCodecAV1:
      if (StaticPrefs::media_webrtc_simulcast_av1_enabled()) {
        return std::make_unique<webrtc::SimulcastEncoderAdapter>(
            aEnv, mInternalFactory.get(), nullptr, aFormat);
      }
      break;
    case webrtc::VideoCodecType::kVideoCodecH264:
      if (StaticPrefs::media_webrtc_simulcast_h264_enabled()) {
        return std::make_unique<webrtc::SimulcastEncoderAdapter>(
            aEnv, mInternalFactory.get(), nullptr, aFormat);
      }
      break;
    case webrtc::VideoCodecType::kVideoCodecVP8:
      return std::make_unique<webrtc::SimulcastEncoderAdapter>(
          aEnv, mInternalFactory.get(), nullptr, aFormat);
    case webrtc::VideoCodecType::kVideoCodecVP9:
      if (StaticPrefs::media_webrtc_simulcast_vp9_enabled()) {
        return std::make_unique<webrtc::SimulcastEncoderAdapter>(
            aEnv, mInternalFactory.get(), nullptr, aFormat);
      }
      break;
  }
  return mInternalFactory->Create(aEnv, aFormat);
}

bool WebrtcVideoEncoderFactory::InternalFactory::Supports(
    const webrtc::SdpVideoFormat& aFormat) {
  switch (webrtc::PayloadStringToCodecType(aFormat.name)) {
    case webrtc::VideoCodecType::kVideoCodecVP8:
    case webrtc::VideoCodecType::kVideoCodecVP9:
    case webrtc::VideoCodecType::kVideoCodecH264:
    case webrtc::VideoCodecType::kVideoCodecAV1:
      return true;
    default:
      return false;
  }
}

std::unique_ptr<webrtc::VideoEncoder>
WebrtcVideoEncoderFactory::InternalFactory::Create(
    const webrtc::Environment& aEnv, const webrtc::SdpVideoFormat& aFormat) {
  MOZ_ASSERT(Supports(aFormat));

  std::unique_ptr<webrtc::VideoEncoder> platformEncoder;

  auto createPlatformEncoder = [&]() -> std::unique_ptr<webrtc::VideoEncoder> {
    return MediaDataCodec::CreateEncoder(aFormat);
  };

  auto createWebRTCEncoder =
      [this, &aEnv, &aFormat]() -> std::unique_ptr<webrtc::VideoEncoder> {
    std::unique_ptr<webrtc::VideoEncoder> encoder;
    switch (webrtc::PayloadStringToCodecType(aFormat.name)) {
      case webrtc::VideoCodecType::kVideoCodecH264: {
        // get an external encoder
        auto gmpEncoder = GmpVideoCodec::CreateEncoder(aFormat, mPCHandle);
        {
          MutexAutoLock lock(mGmpPluginMutex);
          mCreatedGmpPluginEvent.Forward(*gmpEncoder->InitPluginEvent());
          mReleasedGmpPluginEvent.Forward(*gmpEncoder->ReleasePluginEvent());
        }
        encoder = std::move(gmpEncoder);
        break;
      }
      // libvpx fallbacks.
      case webrtc::VideoCodecType::kVideoCodecVP8:
        encoder = webrtc::CreateVp8Encoder(aEnv);
        break;
      case webrtc::VideoCodecType::kVideoCodecVP9:
        encoder = webrtc::CreateVp9Encoder(aEnv);
        break;
      case webrtc::VideoCodecType::kVideoCodecAV1:
        encoder = webrtc::CreateLibaomAv1Encoder(aEnv);
        break;
      default:
        break;
    }
    return encoder;
  };

  std::unique_ptr<webrtc::VideoEncoder> encoder = nullptr;
  EncoderCreationStrategy strategy = static_cast<EncoderCreationStrategy>(
      StaticPrefs::media_webrtc_encoder_creation_strategy());
  switch (strategy) {
    case EncoderCreationStrategy::PreferWebRTCEncoder: {
      encoder = createWebRTCEncoder();
      // In a single case this happens: H264 is requested and OpenH264 isn't
      // available yet (e.g. first run). Attempt to use a platform encoder in
      // this case. They are not entirely ready yet but it's better than
      // erroring out.
      if (!encoder) {
        NS_WARNING(
            "Failed creating libwebrtc video encoder, falling back on platform "
            "encoder");
        LOGD("No libwebrtc encoder for %s; falling back to platform encoder",
             aFormat.name.c_str());
        return createPlatformEncoder();
      }
      LOGD("Created libwebrtc built-in encoder for %s", aFormat.name.c_str());
      return encoder;
    }
    case EncoderCreationStrategy::PreferPlatformEncoder:
      platformEncoder = createPlatformEncoder();
      encoder = createWebRTCEncoder();
      if (encoder && platformEncoder) {
        LOGD(
            "Created platform encoder for %s with libwebrtc software fallback "
            "wrapper",
            aFormat.name.c_str());
        return webrtc::CreateVideoEncoderSoftwareFallbackWrapper(
            aEnv, std::move(encoder), std::move(platformEncoder), false);
      }
      if (platformEncoder) {
        NS_WARNING(nsPrintfCString("No WebRTC encoder to fall back to for "
                                   "codec %s, only using platform encoder",
                                   aFormat.name.c_str())
                       .get());
        LOGD("Created platform encoder for %s (no libwebrtc fallback)",
             aFormat.name.c_str());
        return platformEncoder;
      }
      LOGD("Created libwebrtc encoder for %s (no platform encoder available)",
           aFormat.name.c_str());
      return encoder;
  };

  MOZ_ASSERT_UNREACHABLE("Bad enum value");

  return nullptr;
}

#undef LOGD
#undef LOGTAG

}  // namespace mozilla
