/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <memory>
#include <string>

#include "brave/browser/brave_rewards/rewards_service_factory.h"
#include "brave/common/brave_paths.h"
#include "brave/components/brave_rewards/browser/rewards_service_impl.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_context_helper.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_context_util.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_network_util.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_observer.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_response.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_util.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/testing_profile.h"
#include "chrome/test/base/ui_test_utils.h"
#include "net/dns/mock_host_resolver.h"

// npm run test -- brave_browser_tests --filter=RewardsPublisherBrowserTest.*

namespace rewards_browsertest {

class RewardsPublisherBrowserTest
    : public InProcessBrowserTest {
 public:
  RewardsPublisherBrowserTest() {
    response_ = std::make_unique<RewardsBrowserTestResponse>();
    observer_ = std::make_unique<RewardsBrowserTestObserver>();
  }

  ~RewardsPublisherBrowserTest() override = default;

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();

    // HTTP resolver
    host_resolver()->AddRule("*", "127.0.0.1");
    https_server_.reset(new net::EmbeddedTestServer(
        net::test_server::EmbeddedTestServer::TYPE_HTTPS));
    https_server_->SetSSLConfig(net::EmbeddedTestServer::CERT_OK);
    https_server_->RegisterRequestHandler(
        base::BindRepeating(&rewards_browsertest_util::HandleRequest));
    ASSERT_TRUE(https_server_->Start());

    // Rewards service
    brave::RegisterPathProvider();
    auto* profile = browser()->profile();
    rewards_service_ = static_cast<brave_rewards::RewardsServiceImpl*>(
        brave_rewards::RewardsServiceFactory::GetForProfile(profile));

    // Response mock
    base::ScopedAllowBlockingForTesting allow_blocking;
    response_->LoadMocks();
    rewards_service_->ForTestingSetTestResponseCallback(
        base::BindRepeating(
            &RewardsPublisherBrowserTest::GetTestResponse,
            base::Unretained(this)));

    // Observer
    observer_->Initialize(rewards_service_);
    if (!rewards_service_->IsWalletInitialized()) {
      observer_->WaitForWalletInitialization();
    }
    rewards_service_->SetLedgerEnvForTesting();
  }

  void TearDown() override {
    InProcessBrowserTest::TearDown();
  }

  void GetTestResponse(
      const std::string& url,
      int32_t method,
      int* response_status_code,
      std::string* response,
      std::map<std::string, std::string>* headers) {
    response_->Get(
        url,
        method,
        response_status_code,
        response);
  }

  std::unique_ptr<RewardsBrowserTestResponse> response_;
  std::unique_ptr<net::EmbeddedTestServer> https_server_;
  brave_rewards::RewardsServiceImpl* rewards_service_;
  std::unique_ptr<RewardsBrowserTestObserver> observer_;
};

IN_PROC_BROWSER_TEST_F(
    RewardsPublisherBrowserTest,
    PanelShowsCorrectPublisherData) {
  rewards_browsertest_util::EnableRewardsViaCode(browser(), rewards_service_);

  // Navigate to a verified site in a new tab
  const std::string publisher = "duckduckgo.com";
  GURL url = https_server_->GetURL(publisher, "/index.html");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(),
      url,
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Open the Rewards popup
  content::WebContents* popup_contents =
      rewards_browsertest_helper::OpenRewardsPopup(browser());
  ASSERT_TRUE(popup_contents);

  // Retrieve the inner text of the wallet panel and verify that it
  // looks as expected
  rewards_browsertest_util::WaitForElementToContain(
      popup_contents,
      "[id='wallet-panel']",
      "Brave Verified Creator");
  rewards_browsertest_util::WaitForElementToContain(
      popup_contents,
      "[id='wallet-panel']",
      publisher);

  // Retrieve the inner HTML of the wallet panel and verify that it
  // contains the expected favicon
  {
    const std::string favicon =
        "chrome://favicon/size/64@1x/https://" + publisher;
    rewards_browsertest_util::WaitForElementToContainHTML(
        popup_contents,
        "#wallet-panel",
        favicon);
  }
}

// TODO
//IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, VisitVerifiedPublisher) {
//  // Enable Rewards
//  rewards_browsertest_helper::EnableRewards(browser());
//
//  VisitPublisher("duckduckgo.com", true);
//}
//
//IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, VisitUnverifiedPublisher) {
//  // Enable Rewards
//  rewards_browsertest_helper::EnableRewards(browser());
//
//  VisitPublisher("brave.com", false);
//}

}  // namespace rewards_browsertest
