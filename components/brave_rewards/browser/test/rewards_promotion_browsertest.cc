/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "brave/browser/brave_rewards/rewards_service_factory.h"
#include "brave/common/brave_paths.h"
#include "brave/components/brave_rewards/browser/rewards_service_impl.h"
#include "brave/components/brave_rewards/browser/rewards_service_observer.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/testing_profile.h"

// npm run test -- brave_browser_tests --filter=RewardsPromotionBrowserTest.*

class RewardsPromotionBrowserTest
    : public InProcessBrowserTest,
      public brave_rewards::RewardsServiceObserver,
      public base::SupportsWeakPtr<RewardsBrowserTest> {
 public:
  RewardsPromotionBrowserTest() = default;

  ~RewardsPromotionBrowserTest() override {
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();

    brave::RegisterPathProvider();

    profile_ = browser()->profile();

    rewards_service_ = static_cast<brave_rewards::RewardsServiceImpl*>(
        brave_rewards::RewardsServiceFactory::GetForProfile(profile_));

    rewards_service_->AddObserver(this);
    if (!rewards_service_->IsWalletInitialized()) {
      WaitForWalletInitialization();
    }
    rewards_service_->SetLedgerEnvForTesting();
  }

  void TearDown() override {
    InProcessBrowserTest::TearDown();
  }
};
