/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/files/file_util.h"
#include "base/path_service.h"

#include "chrome/test/base/ui_test_utils.h"
#include "brave/common/brave_paths.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_util.h"

namespace rewards_browsertest_util {

void GetTestDataDir(base::FilePath* test_data_dir) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  ASSERT_TRUE(base::PathService::Get(brave::DIR_TEST_DATA, test_data_dir));
  *test_data_dir = test_data_dir->AppendASCII("rewards-data");
  ASSERT_TRUE(base::PathExists(*test_data_dir));
}

}  // namespace rewards_browsertest_util
