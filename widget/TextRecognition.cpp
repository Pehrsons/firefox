/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TextRecognition.h"

#include "imgIContainer.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/ShadowRoot.h"
#include "nsTextNode.h"

using namespace mozilla::dom;

namespace mozilla::widget {

auto TextRecognition::FindText(imgIContainer& aImage,
                               const nsTArray<nsCString>& aLanguages)
    -> RefPtr<NativePromise> {
  // TODO: Maybe decode async.
  RefPtr<gfx::SourceSurface> surface = aImage.GetFrame(
      imgIContainer::FRAME_CURRENT,
      imgIContainer::FLAG_SYNC_DECODE | imgIContainer::FLAG_ASYNC_NOTIFY);
  if (NS_WARN_IF(!surface)) {
    return NativePromise::CreateAndReject("Failed to get surface"_ns, __func__);
  }
  RefPtr<gfx::DataSourceSurface> dataSurface = surface->GetDataSurface();
  if (NS_WARN_IF(!dataSurface)) {
    return NativePromise::CreateAndReject("Failed to get data surface"_ns,
                                          __func__);
  }
  return FindText(*dataSurface, aLanguages);
}

auto TextRecognition::FindText(gfx::DataSourceSurface& aSurface,
                               const nsTArray<nsCString>& aLanguages)
    -> RefPtr<NativePromise> {
  if (!IsSupported()) {
    return NativePromise::CreateAndReject("Text recognition not available"_ns,
                                          __func__);
  }

  if (XRE_IsContentProcess()) {
    auto* contentChild = ContentChild::GetSingleton();
    auto image = nsContentUtils::SurfaceToIPCImage(aSurface);
    if (!image) {
      return NativePromise::CreateAndReject("Failed to share data surface"_ns,
                                            __func__);
    }
    return contentChild->SendFindImageText(std::move(*image), aLanguages)
        ->Then(
            GetCurrentSerialEventTarget(), __func__,
            [](TextRecognitionResultOrError&& aResultOrError) {
              switch (aResultOrError.type()) {
                case TextRecognitionResultOrError::Type::TTextRecognitionResult:
                  return NativePromise::CreateAndResolve(
                      std::move(aResultOrError.get_TextRecognitionResult()),
                      __func__);
                case TextRecognitionResultOrError::Type::TnsCString:
                  return NativePromise::CreateAndReject(
                      std::move(aResultOrError.get_nsCString()), __func__);
                default:
                  MOZ_ASSERT_UNREACHABLE("Unknown result?");
                  return NativePromise::CreateAndReject("Unknown error"_ns,
                                                        __func__);
              }
            },
            [](mozilla::ipc::ResponseRejectReason) {
              return NativePromise::CreateAndReject("IPC rejection"_ns,
                                                    __func__);
            });
  }
  return DoFindText(aSurface, aLanguages);
}

#ifndef XP_MACOSX
auto TextRecognition::DoFindText(gfx::DataSourceSurface&,
                                 const nsTArray<nsCString>&)
    -> RefPtr<NativePromise> {
  MOZ_CRASH("DoFindText is not implemented on this platform");
}
#endif

bool TextRecognition::IsSupported() {
#ifdef XP_MACOSX
  return true;
#else
  return false;
#endif
}

}  // namespace mozilla::widget
