/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_gtest_ScopedPrefSetter_h
#define mozilla_gtest_ScopedPrefSetter_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Preferences.h"
#include "nsThreadUtils.h"

namespace mozilla {

// Sets a preference for the lifetime of the instance and restores the
// previous value on destruction, so a test does not leak its pref change into
// later tests. Must be constructed and destroyed on the main thread.
template <typename T>
class MOZ_RAII ScopedPrefSetter {
 private:
  static nsresult SetPref(const char* aName, T aValue,
                          PrefValueKind aKind = PrefValueKind::User) {
    if constexpr (std::is_same_v<T, bool>) {
      return Preferences::SetBool(aName, aValue, aKind);
    } else if constexpr (std::is_same_v<T, uint32_t>) {
      return Preferences::SetUint(aName, aValue, aKind);
    } else {
      static_assert(false, "Type not supported with SetPref");
    }
  }

  static T GetPref(const char* aName, T aFallback,
                   PrefValueKind aKind = PrefValueKind::User) {
    if constexpr (std::is_same_v<T, bool>) {
      return Preferences::GetBool(aName, aFallback, aKind);
    } else if constexpr (std::is_same_v<T, uint32_t>) {
      return Preferences::GetUint(aName, aFallback, aKind);
    } else {
      static_assert(false, "Type not supported with GetPref");
    }
  }

 public:
  ScopedPrefSetter(const char* aPrefName, T aValue)
      : mPrefName(aPrefName), mOriginalValue(GetPref(aPrefName, T{})) {
    MOZ_ASSERT(NS_IsMainThread());
    SetPref(mPrefName, aValue);
  }
  ~ScopedPrefSetter() {
    MOZ_ASSERT(NS_IsMainThread());
    SetPref(mPrefName, mOriginalValue);
  }

  ScopedPrefSetter(const ScopedPrefSetter&) = delete;
  ScopedPrefSetter& operator=(const ScopedPrefSetter&) = delete;
  ScopedPrefSetter(ScopedPrefSetter&&) = delete;
  ScopedPrefSetter& operator=(ScopedPrefSetter&&) = delete;

 private:
  const char* mPrefName;
  const T mOriginalValue;
};

}  // namespace mozilla

#endif  // mozilla_gtest_ScopedPrefSetter_h
