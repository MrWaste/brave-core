/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/test/bind_test_util.h"
#include "brave/browser/extensions/api/brave_action_api.h"
#include "brave/browser/ui/views/brave_actions/brave_actions_container.h"
#include "brave/browser/ui/views/location_bar/brave_location_bar_view.h"
#include "brave/common/extensions/extension_constants.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_context_helper.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_context_util.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_util.h"
#include "brave/components/brave_rewards/common/pref_names.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/notification_types.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace rewards_browsertest_helper {

void OpenRewardsPopupRewardsEnabled(Browser* browser) {
  // Ask the popup to open
  std::string error;
  bool popup_shown = extensions::BraveActionAPI::ShowActionUI(
    browser, brave_rewards_extension_id, nullptr, &error);
  if (!popup_shown) {
    LOG(ERROR) << "Could not open rewards popup: " << error;
  }
  EXPECT_TRUE(popup_shown);
}

void OpenRewardsPopupRewardsDisabled(Browser* browser) {
  BrowserView* browser_view =
    BrowserView::GetBrowserViewForBrowser(browser);
  BraveLocationBarView* brave_location_bar_view =
      static_cast<BraveLocationBarView*>(browser_view->GetLocationBarView());
  ASSERT_NE(brave_location_bar_view, nullptr);
  auto* brave_actions = brave_location_bar_view->GetBraveActionsContainer();
  ASSERT_NE(brave_actions, nullptr);

  brave_actions->OnRewardsStubButtonClicked();
}

content::WebContents* OpenRewardsPopup(Browser* browser) {
  // Construct an observer to wait for the popup to load
  content::WebContents* popup_contents = nullptr;
  auto check_load_is_rewards_panel =
      [&](const content::NotificationSource& source,
          const content::NotificationDetails&) -> bool {
        auto web_contents_source =
            static_cast<const content::Source<content::WebContents>&>(source);
        popup_contents = web_contents_source.ptr();

        // Check that this notification is for the Rewards panel and not, say,
        // the extension background page.
        std::string url = popup_contents->GetLastCommittedURL().spec();
        std::string rewards_panel_url = std::string("chrome-extension://") +
            brave_rewards_extension_id + "/brave_rewards_panel.html";
        return url == rewards_panel_url;
      };

  content::WindowedNotificationObserver popup_observer(
      content::NOTIFICATION_LOAD_COMPLETED_MAIN_FRAME,
      base::BindLambdaForTesting(check_load_is_rewards_panel));

  bool rewards_enabled = browser->profile()->GetPrefs()->
      GetBoolean(brave_rewards::prefs::kBraveRewardsEnabled);

  if (rewards_enabled) {
    OpenRewardsPopupRewardsEnabled(browser);
  } else {
    OpenRewardsPopupRewardsDisabled(browser);
  }

  // Wait for the popup to load
  popup_observer.Wait();
  rewards_browsertest_util::WaitForElementToAppear(
      popup_contents,
      "[data-test-id='rewards-panel']");

  return popup_contents;
}

void EnableRewards(Browser* browser, const bool use_new_tab) {
  // Load rewards page
  GURL page_url =
      use_new_tab
      ? rewards_browsertest_util::GetNewTabUrl()
      : rewards_browsertest_util::GetRewardsUrl();
  ui_test_utils::NavigateToURL(browser, page_url);
  auto* contents = browser->tab_strip_model()->GetActiveWebContents();
  WaitForLoadStop(contents);

  // Opt in and create wallet to enable rewards
  rewards_browsertest_util::WaitForElementThenClick(
      contents,
      "[data-test-id='optInAction']");
  rewards_browsertest_util::WaitForElementToAppear(
      contents,
      "[data-test-id2='enableMain']");
}

}  // namespace rewards_browsertest_helper
