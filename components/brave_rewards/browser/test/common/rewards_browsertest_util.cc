/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/files/file_util.h"
#include "base/path_service.h"

#include "base/run_loop.h"
#include "base/test/bind_test_util.h"
#include "bat/ledger/mojom_structs.h"
#include "brave/common/brave_paths.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_util.h"
#include "brave/components/brave_rewards/common/pref_names.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/prefs/pref_service.h"

namespace rewards_browsertest_util {

void GetTestDataDir(base::FilePath* test_data_dir) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  ASSERT_TRUE(base::PathService::Get(brave::DIR_TEST_DATA, test_data_dir));
  *test_data_dir = test_data_dir->AppendASCII("rewards-data");
  ASSERT_TRUE(base::PathExists(*test_data_dir));
}

double IsRewardsEnabled(Browser* browser, const bool private_window) {
  DCHECK(browser);
  auto* profile = browser->profile();
  if (private_window) {
    Profile* private_profile = profile->GetOffTheRecordProfile();
    return private_profile->GetPrefs()->GetBoolean(
      brave_rewards::prefs::kBraveRewardsEnabled);
  }

  return profile->GetPrefs()->GetBoolean(
      brave_rewards::prefs::kBraveRewardsEnabled);
}

void RunUntilIdle() {
    base::RunLoop loop;
    loop.RunUntilIdle();
}

GURL GetRewardsUrl() {
  GURL rewards_url("brave://rewards");
  return rewards_url;
}

GURL GetNewTabUrl() {
  GURL new_tab_url("brave://newtab");
  return new_tab_url;
}

void EnableRewardsViaCode(
    Browser* browser,
    brave_rewards::RewardsServiceImpl* rewards_service) {
  base::RunLoop run_loop;
  bool wallet_created = false;
  rewards_service->CreateWallet(
      base::BindLambdaForTesting([&](int32_t result) {
        wallet_created =
            (result == static_cast<int32_t>(ledger::Result::WALLET_CREATED));
        run_loop.Quit();
      }));

  run_loop.Run();

  ASSERT_TRUE(wallet_created);
  ASSERT_TRUE(IsRewardsEnabled(browser));
}

}  // namespace rewards_browsertest_util
