/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/brave_rewards/browser/rewards_notification_service.h"
#include "brave/components/brave_rewards/browser/rewards_notification_service_observer.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_util.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_network_util.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_observer.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_response.h"
#include "brave/components/brave_rewards/browser/rewards_service_impl.h"
#include "brave/browser/brave_rewards/rewards_service_factory.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "brave/common/brave_paths.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"

// npm run test -- brave_browser_tests --filter=RewardsNotificationBrowserTest.*
namespace rewards_browsertest {

using RewardsNotificationType =
    brave_rewards::RewardsNotificationService::RewardsNotificationType;

class RewardsNotificationBrowserTest
    : public InProcessBrowserTest,
      public brave_rewards::RewardsNotificationServiceObserver {
 public:
  RewardsNotificationBrowserTest() {
    response_ = std::make_unique<RewardsBrowserTestResponse>();
    observer_ = std::make_unique<RewardsBrowserTestObserver>();
  }

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
            &RewardsNotificationBrowserTest::GetTestResponse,
            base::Unretained(this)));

     // Observer
    observer_->Initialize(rewards_service_);
    if (!rewards_service_->IsWalletInitialized()) {
      observer_->WaitForWalletInitialization();
    }
    rewards_service_->SetLedgerEnvForTesting();

    rewards_notification_service_ = rewards_service_->GetNotificationService();
    rewards_notification_service_->AddObserver(this);
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

  void OnNotificationAdded(
      brave_rewards::RewardsNotificationService* rewards_notification_service,
      const brave_rewards::RewardsNotificationService::RewardsNotification&
      notification) override {
    last_added_notification_ = notification;
    const auto& notifications = rewards_service_->GetAllNotifications();
    for (const auto& notification : notifications) {
      switch (notification.second.type_) {
        case RewardsNotificationType::REWARDS_NOTIFICATION_INSUFFICIENT_FUNDS: {
          insufficient_notification_would_have_already_shown_ = true;
          if (wait_for_insufficient_notification_loop_) {
            wait_for_insufficient_notification_loop_->Quit();
          }
          break;
        }
        default: {
          add_notification_ = true;
          if (wait_for_add_notification_loop_) {
            wait_for_add_notification_loop_->Quit();
          }
          break;
        }
      }
    }
  }

  void OnNotificationDeleted(
      brave_rewards::RewardsNotificationService* rewards_notification_service,
      const brave_rewards::RewardsNotificationService::RewardsNotification&
      notification) override {
    last_deleted_notification_ = notification;
    delete_notification_ = true;
    if (wait_for_delete_notification_loop_) {
      wait_for_delete_notification_loop_->Quit();
    }
  }

  void OnAllNotificationsDeleted(
      brave_rewards::RewardsNotificationService* rewards_notification_service)
      override {
  }

  void OnGetNotification(
      brave_rewards::RewardsNotificationService* rewards_notification_service,
      const brave_rewards::RewardsNotificationService::RewardsNotification&
      notification) override {
  }

  void WaitForAddNotificationCallback() {
    if (add_notification_) {
      return;
    }
    wait_for_add_notification_loop_.reset(new base::RunLoop);
    wait_for_add_notification_loop_->Run();
  }

  void WaitForDeleteNotificationCallback() {
    if (delete_notification_) {
      return;
    }
    wait_for_delete_notification_loop_.reset(new base::RunLoop);
    wait_for_delete_notification_loop_->Run();
  }

  void WaitForInsufficientFundsNotification() {
    if (insufficient_notification_would_have_already_shown_) {
      return;
    }

    wait_for_insufficient_notification_loop_.reset(new base::RunLoop);
    wait_for_insufficient_notification_loop_->Run();
  }

  void CheckInsufficientFundsForTesting() {
    rewards_service_->MaybeShowNotificationAddFundsForTesting(
        base::BindOnce(
            &RewardsNotificationBrowserTest::
            ShowNotificationAddFundsForTesting,
            base::Unretained(this)));
  }

  /**
   * When using notification observer for insufficient funds, tests will fail
   * for sufficient funds because observer will never be called for
   * notification. Use this as callback to know when we come back with
   * sufficient funds to prevent inf loop
   * */
  void ShowNotificationAddFundsForTesting(bool sufficient) {
    if (sufficient) {
      insufficient_notification_would_have_already_shown_ = true;
      if (wait_for_insufficient_notification_loop_) {
        wait_for_insufficient_notification_loop_->Quit();
      }
    }
  }

  bool IsShowingNotificationForType(const RewardsNotificationType type) {
    const auto& notifications = rewards_service_->GetAllNotifications();
    for (const auto& notification : notifications) {
      if (notification.second.type_ == type) {
        return true;
      }
    }

    return false;
  }

  brave_rewards::RewardsNotificationService* rewards_notification_service_;
  brave_rewards::RewardsServiceImpl* rewards_service_;
  std::unique_ptr<RewardsBrowserTestResponse> response_;
  std::unique_ptr<net::EmbeddedTestServer> https_server_;
  std::unique_ptr<RewardsBrowserTestObserver> observer_;

  brave_rewards::RewardsNotificationService::RewardsNotification
    last_added_notification_;
  brave_rewards::RewardsNotificationService::RewardsNotification
    last_deleted_notification_;

  std::unique_ptr<base::RunLoop> wait_for_insufficient_notification_loop_;
  bool insufficient_notification_would_have_already_shown_ = false;

  std::unique_ptr<base::RunLoop> wait_for_add_notification_loop_;
  bool add_notification_ = false;

  std::unique_ptr<base::RunLoop> wait_for_delete_notification_loop_;
  bool delete_notification_ = false;
};

IN_PROC_BROWSER_TEST_F(
    RewardsNotificationBrowserTest,
    AddGrantNotification) {
  brave_rewards::RewardsNotificationService::RewardsNotificationArgs args;
  args.push_back("foo");
  args.push_back("bar");

  rewards_notification_service_->AddNotification(
      brave_rewards::RewardsNotificationService::REWARDS_NOTIFICATION_GRANT,
      args,
      "rewards_notification_grant");
  WaitForAddNotificationCallback();

  EXPECT_EQ(last_added_notification_.args_.size(), 2ul);
  EXPECT_STREQ(last_added_notification_.args_.at(0).c_str(), "foo");
  EXPECT_STREQ(last_added_notification_.args_.at(1).c_str(), "bar");

  EXPECT_STREQ(
      last_added_notification_.id_.c_str(),
      "rewards_notification_grant");
  EXPECT_NE(last_added_notification_.timestamp_, 0ul);
}

IN_PROC_BROWSER_TEST_F(
    RewardsNotificationBrowserTest,
    AddGrantNotificationAndDeleteIt) {
  brave_rewards::RewardsNotificationService::RewardsNotificationArgs args;
  args.push_back("foo");
  args.push_back("bar");

  rewards_notification_service_->AddNotification(
      brave_rewards::RewardsNotificationService::REWARDS_NOTIFICATION_GRANT,
      args,
      "rewards_notification_grant");
  WaitForAddNotificationCallback();

  EXPECT_STREQ(
      last_added_notification_.id_.c_str(),
      "rewards_notification_grant");

  rewards_notification_service_->DeleteNotification(
      last_added_notification_.id_);
  WaitForDeleteNotificationCallback();
  EXPECT_STREQ(
      last_deleted_notification_.id_.c_str(),
      "rewards_notification_grant");
  EXPECT_NE(last_deleted_notification_.timestamp_,  0ul);
}

IN_PROC_BROWSER_TEST_F(
    RewardsNotificationBrowserTest,
    AddGrantNotificationAndFakeItAndDeleteIt) {
  brave_rewards::RewardsNotificationService::RewardsNotificationArgs args;
  args.push_back("foo");
  args.push_back("bar");

  rewards_notification_service_->AddNotification(
      brave_rewards::RewardsNotificationService::REWARDS_NOTIFICATION_GRANT,
      args,
      "rewards_notification_grant");
  WaitForAddNotificationCallback();

  EXPECT_STREQ(
      last_added_notification_.id_.c_str(),
      "rewards_notification_grant");

  rewards_notification_service_->DeleteNotification("not_valid");
  WaitForDeleteNotificationCallback();
  EXPECT_TRUE(
      last_deleted_notification_.type_ ==
      brave_rewards::RewardsNotificationService::REWARDS_NOTIFICATION_INVALID);
}

IN_PROC_BROWSER_TEST_F(
    RewardsNotificationBrowserTest,
    InsufficientNotificationForZeroAmountZeroPublishers) {
  rewards_browsertest_util::EnableRewardsViaCode(browser(), rewards_service_);
  CheckInsufficientFundsForTesting();
  WaitForInsufficientFundsNotification();
  const auto& notifications = rewards_service_->GetAllNotifications();

  if (notifications.empty()) {
    SUCCEED();
    return;
  }

  bool is_showing_notification = IsShowingNotificationForType(
      RewardsNotificationType::REWARDS_NOTIFICATION_INSUFFICIENT_FUNDS);

  EXPECT_FALSE(is_showing_notification);
}

// TODO
//IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
//                       InsufficientNotificationForACNotEnoughFunds) {
//  rewards_browsertest_helper::EnableRewards(browser());
//
//  // Visit publishers
//  const bool verified = true;
//  while (!last_publisher_added_) {
//    VisitPublisher("duckduckgo.com", verified);
//    VisitPublisher("bumpsmack.com", verified);
//    VisitPublisher("brave.com", !verified, true);
//  }
//
//  CheckInsufficientFundsForTesting();
//  WaitForInsufficientFundsNotification();
//  const brave_rewards::RewardsNotificationService::RewardsNotificationsMap&
//      notifications = rewards_service_->GetAllNotifications();
//
//  if (notifications.empty()) {
//    SUCCEED();
//    return;
//  }
//
//  bool is_showing_notification = IsShowingNotificationForType(
//      RewardsNotificationType::REWARDS_NOTIFICATION_INSUFFICIENT_FUNDS);
//
//  EXPECT_FALSE(is_showing_notification);
//}
//
//IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
//                       InsufficientNotificationForInsufficientAmount) {
//  rewards_browsertest_helper::EnableRewards(browser());
//  balance_ = promotion_->ClaimPromotionViaCode();
//
//  TipViaCode(
//      "duckduckgo.com",
//      20.0,
//      ledger::PublisherStatus::VERIFIED,
//      false,
//      true);
//
//  TipViaCode(
//      "brave.com",
//      50.0,
//      ledger::PublisherStatus::NOT_VERIFIED,
//      false,
//      true);
//
//  CheckInsufficientFundsForTesting();
//  WaitForInsufficientFundsNotification();
//  const brave_rewards::RewardsNotificationService::RewardsNotificationsMap&
//      notifications = rewards_service_->GetAllNotifications();
//
//  if (notifications.empty()) {
//    SUCCEED();
//    return;
//  }
//
//  bool is_showing_notification = IsShowingNotificationForType(
//      RewardsNotificationType::REWARDS_NOTIFICATION_INSUFFICIENT_FUNDS);
//
//  EXPECT_FALSE(is_showing_notification);
//}
//
//IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
//                       InsufficientNotificationForVerifiedInsufficientAmount) {
//  rewards_browsertest_helper::EnableRewards(browser());
//  balance_ = promotion_->ClaimPromotionViaCode();
//
//  TipViaCode(
//      "duckduckgo.com",
//      50.0,
//      ledger::PublisherStatus::VERIFIED,
//      false,
//      true);
//
//  TipViaCode(
//      "brave.com",
//      50.0,
//      ledger::PublisherStatus::NOT_VERIFIED,
//      false,
//      true);
//
//  CheckInsufficientFundsForTesting();
//  WaitForInsufficientFundsNotification();
//  const brave_rewards::RewardsNotificationService::RewardsNotificationsMap&
//      notifications = rewards_service_->GetAllNotifications();
//
//  if (notifications.empty()) {
//    FAIL() << "Should see Insufficient Funds notification";
//    return;
//  }
//
//  bool is_showing_notification = IsShowingNotificationForType(
//      RewardsNotificationType::REWARDS_NOTIFICATION_INSUFFICIENT_FUNDS);
//
//  EXPECT_TRUE(is_showing_notification);
//}

}  // namespace rewards_browsertest
