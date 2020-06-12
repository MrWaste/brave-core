/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_CHROMIUM_SRC_COMPONENTS_PREFS_PREF_SERVICE_H_
#define BRAVE_CHROMIUM_SRC_COMPONENTS_PREFS_PREF_SERVICE_H_

#include "../../../../components/prefs/pref_service.h"
#include "base/memory/singleton.h"

namespace prefs {
// Allows access to active user's profile PrefService in components
class COMPONENTS_PREFS_EXPORT BravePrefService {
 public:
  static BravePrefService* GetInstance() {
    return base::Singleton<BravePrefService>::get();
  }

  void RegisterPrefService(PrefService* pref_service) {
    pref_service_ = pref_service;
  }

  PrefService* GetPrefs() { return pref_service_; }

 private:
  friend struct base::DefaultSingletonTraits<BravePrefService>;

  BravePrefService() {}
  ~BravePrefService() {}

  PrefService* pref_service_;

  DISALLOW_COPY_AND_ASSIGN(BravePrefService);
};
}  // namespace prefs

#endif  // BRAVE_CHROMIUM_SRC_COMPONENTS_PREFS_PREF_SERVICE_H_
