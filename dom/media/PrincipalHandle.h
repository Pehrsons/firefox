/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_PRINCIPALHANDLE_H_
#define DOM_MEDIA_PRINCIPALHANDLE_H_

#include "mozilla/Atomics.h"
#include "nsIPrincipal.h"
#include "nsProxyRelease.h"

namespace mozilla {
/**
 * The level of privacy of a principal as considered by RTCPeerConnection.
 */
enum class PrincipalPrivacy : uint8_t { Private, NonPrivate };

/**
 * We pass the principal through the MediaTrackGraph by wrapping it in a thread
 * safe nsMainThreadPtrHandle, since it cannot be used directly off the main
 * thread. We can compare two PrincipalHandles to each other on any thread, but
 * they can only be created and converted back to nsIPrincipal* on main thread.
 *
 * Each handle constructed from a principal has a unique generation id, and
 * handles are equal only if they share it. Two handles for the same principal
 * made separately therefore differ, so that a change away from a principal and
 * back again is not coalesced away by state mirroring or chunk comparisons.
 */
struct PrincipalHandle {
  explicit PrincipalHandle(std::nullptr_t) {}
  explicit PrincipalHandle(nsIPrincipal* aPrincipal)
      : mPrincipal(new nsMainThreadPtrHolder<nsIPrincipal>(
            "PrincipalHandle::mPrincipal", aPrincipal)),
        mGenerationId(sNextGenerationId++) {}

  bool operator==(const PrincipalHandle& aOther) const {
    return mGenerationId == aOther.mGenerationId;
  }
  bool operator!=(const PrincipalHandle& aOther) const {
    return !(*this == aOther);
  }

  nsMainThreadPtrHandle<nsIPrincipal> mPrincipal;
  // 0 for no principal.
  size_t mGenerationId = 0;

 private:
  static inline Atomic<size_t> sNextGenerationId{1};
};

#define PRINCIPAL_HANDLE_NONE PrincipalHandle(nullptr)

inline nsIPrincipal* GetPrincipalFromHandle(
    const PrincipalHandle& aPrincipalHandle) {
  MOZ_ASSERT(NS_IsMainThread());
  return aPrincipalHandle.mPrincipal.get();
}

inline bool PrincipalHandleMatches(const PrincipalHandle& aPrincipalHandle,
                                   nsIPrincipal* aOther) {
  if (!aOther) {
    return false;
  }

  nsIPrincipal* principal = GetPrincipalFromHandle(aPrincipalHandle);
  if (!principal) {
    return false;
  }

  bool result;
  if (NS_FAILED(principal->Equals(aOther, &result))) {
    NS_ERROR("Principal check failed");
    return false;
  }

  return result;
}
}  // namespace mozilla

#endif  // DOM_MEDIA_PRINCIPALHANDLE_H_
