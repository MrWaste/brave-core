/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/strings/string_split.h"
#include "base/strings/stringprintf.h"
#include "base/test/bind_test_util.h"
#include "bat/ledger/internal/request/request_sku.h"
#include "bat/ledger/internal/request/request_util.h"
#include "bat/ledger/internal/static_values.h"
#include "bat/ledger/internal/uphold/uphold_util.h"
#include "bat/ledger/mojom_structs.h"
#include "brave/browser/brave_rewards/rewards_service_factory.h"
#include "brave/browser/extensions/api/brave_action_api.h"
#include "brave/browser/ui/views/brave_actions/brave_actions_container.h"
#include "brave/browser/ui/views/location_bar/brave_location_bar_view.h"
#include "brave/common/brave_paths.h"
#include "brave/common/extensions/extension_constants.h"
#include "brave/components/brave_rewards/browser/rewards_notification_service.h"
#include "brave/components/brave_rewards/browser/rewards_notification_service_observer.h"
#include "brave/components/brave_rewards/browser/rewards_service_impl.h"
#include "brave/components/brave_rewards/browser/rewards_service_observer.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_context_helper.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_context_util.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_network_util.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_promotion.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_observer.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_response.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_util.h"
#include "brave/components/brave_rewards/common/pref_names.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/network_session_configurator/common/network_switches.h"
#include "content/public/browser/notification_types.h"
#include "net/dns/mock_host_resolver.h"
#include "testing/gmock/include/gmock/gmock.h"

// npm run test -- brave_browser_tests --filter=RewardsBrowserTest.*

namespace rewards_browsertest {

using RewardsNotificationType =
    brave_rewards::RewardsNotificationService::RewardsNotificationType;

class RewardsBrowserTest
    : public InProcessBrowserTest,
      public brave_rewards::RewardsServiceObserver,
      public brave_rewards::RewardsNotificationServiceObserver,
      public base::SupportsWeakPtr<RewardsBrowserTest> {
 public:
  RewardsBrowserTest() {
    // You can do set-up work for each test here
    response_ = std::make_unique<RewardsBrowserTestResponse>();
    promotion_ = std::make_unique<RewardsBrowserTestPromotion>();
    observer_ = std::make_unique<RewardsBrowserTestObserver>();
  }

  ~RewardsBrowserTest() override {
    // You can do clean-up work that doesn't throw exceptions here
  }

  void SetUpOnMainThread() override {
    // Code here will be called immediately after the constructor (right before
    // each test)

    InProcessBrowserTest::SetUpOnMainThread();

    host_resolver()->AddRule("*", "127.0.0.1");

    // Setup up embedded test server for HTTPS requests
    https_server_.reset(new net::EmbeddedTestServer(
        net::test_server::EmbeddedTestServer::TYPE_HTTPS));
    https_server_->SetSSLConfig(net::EmbeddedTestServer::CERT_OK);
    https_server_->RegisterRequestHandler(
        base::BindRepeating(&rewards_browsertest_util::HandleRequest));
    ASSERT_TRUE(https_server_->Start());

    brave::RegisterPathProvider();
    base::ScopedAllowBlockingForTesting allow_blocking;
    response_->LoadMocks();

    auto* browser_profile = browser()->profile();

    rewards_service_ = static_cast<brave_rewards::RewardsServiceImpl*>(
        brave_rewards::RewardsServiceFactory::GetForProfile(browser_profile));
    rewards_service_->ForTestingSetTestResponseCallback(
        base::BindRepeating(&RewardsBrowserTest::GetTestResponse,
                            base::Unretained(this)));
    rewards_service_->AddObserver(this);
    observer_->Initialize(rewards_service_);
    if (!rewards_service_->IsWalletInitialized()) {
      observer_->WaitForWalletInitialization();
    }
    rewards_service_->SetLedgerEnvForTesting();

    promotion_->Initialize(browser(), rewards_service_);
  }

  void TearDown() override {
    // Code here will be called immediately after each test (right before the
    // destructor)
    InProcessBrowserTest::TearDown();
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    // HTTPS server only serves a valid cert for localhost, so this is needed
    // to load pages from other hosts without an error
    command_line->AppendSwitch(switches::kIgnoreCertificateErrors);
  }

  net::EmbeddedTestServer* https_server() {
    return https_server_.get();
  }

  void GetTestResponse(
      const std::string& url,
      int32_t method,
      int* response_status_code,
      std::string* response,
      std::map<std::string, std::string>* headers) {
    response_->SetExternalBalance(GetExternalBalance());
    response_->Get(
        url,
        method,
        response_status_code,
        response);
  }

  void WaitForPublisherListNormalized() {
    if (publisher_list_normalized_) {
      return;
    }
    wait_for_publisher_list_normalized_loop_.reset(new base::RunLoop);
    wait_for_publisher_list_normalized_loop_->Run();
  }

  void WaitForACReconcileCompleted() {
    if (ac_reconcile_completed_) {
      return;
    }
    wait_for_ac_completed_loop_.reset(new base::RunLoop);
    wait_for_ac_completed_loop_->Run();
  }

  void WaitForTipReconcileCompleted() {
    if (tip_reconcile_completed_) {
      return;
    }
    wait_for_tip_completed_loop_.reset(new base::RunLoop);
    wait_for_tip_completed_loop_->Run();
  }

  void WaitForPendingTipToBeSaved() {
    if (pending_tip_saved_) {
      return;
    }
    wait_for_pending_tip_saved_loop_.reset(new base::RunLoop);
    wait_for_pending_tip_saved_loop_->Run();
  }

  void WaitForMultipleTipReconcileCompleted(int32_t needed) {
    multiple_tip_reconcile_needed_ = needed;
    if (multiple_tip_reconcile_completed_||
        multiple_tip_reconcile_count_ == needed) {
      return;
    }

    wait_for_multiple_tip_completed_loop_.reset(new base::RunLoop);
    wait_for_multiple_tip_completed_loop_->Run();
  }

  void WaitForMultipleACReconcileCompleted(int32_t needed) {
    multiple_ac_reconcile_needed_ = needed;
    if (multiple_ac_reconcile_completed_ ||
        multiple_ac_reconcile_count_ == needed) {
      return;
    }

    wait_for_multiple_ac_completed_loop_.reset(new base::RunLoop);
    wait_for_multiple_ac_completed_loop_->Run();
  }

  void WaitForRecurringTipToBeSaved() {
    if (recurring_tip_saved_) {
      return;
    }
    wait_for_recurring_tip_saved_loop_.reset(new base::RunLoop);
    wait_for_recurring_tip_saved_loop_->Run();
  }

  void UpdateContributionBalance(
      double amount,
      bool verified = false,
      const ledger::ContributionProcessor processor =
          ledger::ContributionProcessor::BRAVE_TOKENS) {

    if (verified) {
      if (processor == ledger::ContributionProcessor::BRAVE_TOKENS ||
          processor == ledger::ContributionProcessor::BRAVE_USER_FUNDS) {
        balance_ -= amount;
        return;
      }

      if (processor == ledger::ContributionProcessor::UPHOLD) {
        external_balance_ -= amount;
        return;
      }

      return;
    }

    pending_balance_ += amount;
  }

  static std::string BalanceDoubleToString(double amount) {
    return base::StringPrintf("%.3f", amount);
  }

  std::string GetBalance() const {
    return BalanceDoubleToString(balance_ + external_balance_);
  }

  std::string GetPendingBalance() const {
    return BalanceDoubleToString(pending_balance_);
  }

  std::string GetExternalBalance() {
    return BalanceDoubleToString(external_balance_);
  }

  std::string GetAnonBalance() const {
    return BalanceDoubleToString(balance_);
  }

  content::WebContents* contents() const {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  brave_rewards::RewardsServiceImpl* rewards_service() {
    return rewards_service_;
  }

  std::string RewardsPageTipSummaryAmount() const {
    const std::string amount =
        rewards_browsertest_util::WaitForElementThenGetContent(
        contents(),
        "[data-test-id=summary-tips] [color=contribute] span span");
    return amount + " BAT";
  }

  std::string ExpectedTipSummaryAmountString() const {
    // The tip summary page formats 2.4999 as 2.4, so we do the same here.
    double truncated_amount = floor(reconciled_tip_total_ * 10) / 10;
    return BalanceDoubleToString(-truncated_amount);
  }

  void RefreshPublisherListUsingRewardsPopup() const {
    rewards_browsertest_util::WaitForElementThenClick(
        rewards_browsertest_helper::OpenRewardsPopup(browser()),
        "[data-test-id='unverified-check-button']");
  }

  void TipPublisher(
      const std::string& publisher,
      rewards_browsertest_util::ContributionType type,
      bool should_contribute = false,
      int32_t selection = 0,
      int32_t number_of_contributions = 1) {
    // we shouldn't be adding publisher to AC list,
    // so that we can focus only on tipping part
    rewards_service_->SetPublisherMinVisitTime(8);

    // Navigate to a site in a new tab
    GURL url = https_server()->GetURL(publisher, "/index.html");
    ui_test_utils::NavigateToURLWithDisposition(
        browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

    content::WebContents* site_banner_contents =
        rewards_browsertest_helper::OpenSiteBanner(browser(), type);
    ASSERT_TRUE(site_banner_contents);

    auto tip_options = rewards_browsertest_util::GetSiteBannerTipOptions(
            site_banner_contents);
    const double amount = tip_options.at(selection);
    const std::string amount_str = base::StringPrintf("%.3f", amount);

    // Select the tip amount (default is 1.000 BAT)
    std::string amount_selector = base::StringPrintf(
        "div:nth-of-type(%u)>[data-test-id=amount-wrapper]",
        selection + 1);
    rewards_browsertest_util::WaitForElementThenClick(
        site_banner_contents,
        amount_selector);

    // Send the tip
    rewards_browsertest_util::WaitForElementThenClick(
        site_banner_contents,
        "[data-test-id='send-tip-button']");

    // Signal that direct tip was made and update wallet with new
    // balance
    if (type == rewards_browsertest_util::ContributionType::OneTimeTip &&
        !should_contribute) {
      WaitForPendingTipToBeSaved();
      UpdateContributionBalance(amount, should_contribute);
    }

    // Wait for thank you banner to load
    ASSERT_TRUE(WaitForLoadStop(site_banner_contents));

    const std::string confirmationText =
        type == rewards_browsertest_util::ContributionType::MonthlyTip
        ? "Monthly contribution has been set!"
        : "Tip sent!";

    if (type == rewards_browsertest_util::ContributionType::MonthlyTip) {
      WaitForRecurringTipToBeSaved();
      // Trigger contribution process
      rewards_service()->StartMonthlyContributionForTest();

      // Wait for reconciliation to complete
      if (should_contribute) {
        WaitForTipReconcileCompleted();
        const auto result = should_contribute
            ? ledger::Result::LEDGER_OK
            : ledger::Result::RECURRING_TABLE_EMPTY;
        ASSERT_EQ(tip_reconcile_status_, result);
      }

      // Signal that monthly contribution was made and update wallet
      // with new balance
      if (!should_contribute) {
        UpdateContributionBalance(amount, should_contribute);
      }
    } else if (type == rewards_browsertest_util::ContributionType::OneTimeTip &&
        should_contribute) {
      // Wait for reconciliation to complete
      WaitForMultipleTipReconcileCompleted(number_of_contributions);
      ASSERT_EQ(multiple_tip_reconcile_status_, ledger::Result::LEDGER_OK);
    }

    // Make sure that thank you banner shows correct publisher data
    // (domain and amount)
    {
      rewards_browsertest_util::WaitForElementToContain(
          site_banner_contents,
          "body",
          confirmationText);
      rewards_browsertest_util::WaitForElementToContain(
          site_banner_contents,
          "body",
          amount_str + " BAT");
      rewards_browsertest_util::WaitForElementToContain(
          site_banner_contents,
          "body",
          "Share the good news:");
      rewards_browsertest_util::WaitForElementToContain(
          site_banner_contents,
          "body",
          "" + GetBalance() + " BAT");
    }

    VerifyTip(
        amount,
        should_contribute,
        type == rewards_browsertest_util::ContributionType::MonthlyTip);
  }

  void VerifyTip(
      const double amount,
      const bool should_contribute,
      const bool monthly,
      const bool via_code = false) {
    if (via_code && monthly) {
      return;
    }

    // Activate the Rewards settings page tab
    ActivateTabAtIndex(0);

    if (should_contribute) {
      // Make sure that balance is updated correctly
      IsBalanceCorrect();

      // Check that tip table shows the appropriate tip amount
      const std::string selector = monthly
          ? "[data-test-id='summary-monthly']"
          : "[data-test-id='summary-tips']";

      rewards_browsertest_util::WaitForElementToContain(
          contents(),
          selector,
          "-" + BalanceDoubleToString(amount) + "BAT");
      return;
    }

    // Make sure that balance did not change
    IsBalanceCorrect();

    // Make sure that pending contribution box shows the correct
    // amount
    IsPendingBalanceCorrect();

    rewards_browsertest_util::WaitForElementToEqual(
        contents(),
        "#tip-box-total",
        "0.000BAT0.00 USD");
  }

  void IsBalanceCorrect() {
    const std::string balance = GetBalance() + " BAT";
    rewards_browsertest_util::WaitForElementToEqual(
        contents(),
        "[data-test-id='balance']",
        balance);
  }

  void IsPendingBalanceCorrect() {
    const std::string balance = GetPendingBalance() + " BAT";
    rewards_browsertest_util::WaitForElementToContain(
        contents(),
        "[data-test-id='pending-contribution-box']",
        balance);
  }

  void OnPublisherListNormalized(brave_rewards::RewardsService* rewards_service,
                                 const brave_rewards::ContentSiteList& list) {
    if (list.size() == 0)
      return;
    publisher_list_normalized_ = true;
    if (wait_for_publisher_list_normalized_loop_)
      wait_for_publisher_list_normalized_loop_->Quit();
  }

  void OnReconcileComplete(
      brave_rewards::RewardsService* rewards_service,
      unsigned int result,
      const std::string& contribution_id,
      const double amount,
      const int32_t type,
      const int32_t processor) {
    const auto converted_result = static_cast<ledger::Result>(result);
    const auto converted_type =
        static_cast<ledger::RewardsType>(type);

    if (converted_result == ledger::Result::LEDGER_OK) {
      UpdateContributionBalance(
          amount,
          true,
          static_cast<ledger::ContributionProcessor>(processor));
    }

    if (converted_type == ledger::RewardsType::AUTO_CONTRIBUTE) {
      ac_reconcile_completed_ = true;
      ac_reconcile_status_ = converted_result;
      if (wait_for_ac_completed_loop_) {
        wait_for_ac_completed_loop_->Quit();
      }

      // Multiple ac
      multiple_ac_reconcile_count_++;
      multiple_ac_reconcile_status_.push_back(converted_result);

      if (multiple_ac_reconcile_count_ == multiple_ac_reconcile_needed_) {
        multiple_ac_reconcile_completed_ = true;
        if (wait_for_multiple_ac_completed_loop_) {
          wait_for_multiple_ac_completed_loop_->Quit();
        }
      }
    }

    if (converted_type == ledger::RewardsType::ONE_TIME_TIP ||
        converted_type == ledger::RewardsType::RECURRING_TIP) {
      if (converted_result == ledger::Result::LEDGER_OK) {
        reconciled_tip_total_ += amount;
      }

      // Single tip tracking
      tip_reconcile_completed_ = true;
      tip_reconcile_status_ = converted_result;
      if (wait_for_tip_completed_loop_) {
        wait_for_tip_completed_loop_->Quit();
      }

      // Multiple tips
      multiple_tip_reconcile_count_++;
      multiple_tip_reconcile_status_ = converted_result;

      if (multiple_tip_reconcile_count_ == multiple_tip_reconcile_needed_) {
        multiple_tip_reconcile_completed_ = true;
        if (wait_for_multiple_tip_completed_loop_) {
          wait_for_multiple_tip_completed_loop_->Quit();
        }
      }
    }
  }

  void OnRecurringTipSaved(
      brave_rewards::RewardsService* rewards_service,
      bool success) {
    if (!success) {
      return;
    }

    recurring_tip_saved_ = true;
    if (wait_for_recurring_tip_saved_loop_) {
      wait_for_recurring_tip_saved_loop_->Quit();
    }
  }

  void OnPendingContributionSaved(
      brave_rewards::RewardsService* rewards_service,
      int result) {
    if (result != 0) {
      return;
    }

    pending_tip_saved_ = true;
    if (wait_for_pending_tip_saved_loop_) {
      wait_for_pending_tip_saved_loop_->Quit();
    }
  }

  void TipViaCode(
      const std::string publisher_key,
      const double amount,
      const ledger::PublisherStatus status,
      const bool should_contribute = false,
      const bool recurring = false,
      const ledger::Result result = ledger::Result::LEDGER_OK) {
    tip_reconcile_completed_ = false;
    pending_tip_saved_ = false;

    auto site = std::make_unique<brave_rewards::ContentSite>();
    site->id = publisher_key;
    site->name = publisher_key;
    site->url = publisher_key;
    site->status = static_cast<int>(status);
    site->provider = "";
    site->favicon_url = "";
    rewards_service_->OnTip(publisher_key, amount, recurring, std::move(site));

    if (recurring) {
      return;
    }

    if (should_contribute) {
      // Wait for reconciliation to complete
      WaitForTipReconcileCompleted();
      ASSERT_EQ(tip_reconcile_status_, result);
      return;
    }

    // Signal to update pending contribution balance
    WaitForPendingTipToBeSaved();
    UpdateContributionBalance(amount, should_contribute);
  }

  void SetUpUpholdWallet(
      const double balance,
      const ledger::WalletStatus status = ledger::WalletStatus::VERIFIED) {
    verified_wallet_ = true;
    response_->SetVerifiedWallet(true);
    external_balance_ = balance;

    const std::string external_wallet_address =
      "abe5f454-fedd-4ea9-9203-470ae7315bb3";

    response_->SetUpholdAddress(external_wallet_address);

    auto wallet = ledger::ExternalWallet::New();
    wallet->token = "token";
    wallet->address = external_wallet_address;
    wallet->status = status;
    wallet->one_time_string = "";
    wallet->user_name = "Brave Test";
    wallet->transferred = true;
    rewards_service()->SaveExternalWallet("uphold", std::move(wallet));
  }

  std::unique_ptr<RewardsBrowserTestResponse> response_;
  std::unique_ptr<RewardsBrowserTestPromotion> promotion_;
  std::unique_ptr<RewardsBrowserTestObserver> observer_;

  std::unique_ptr<net::EmbeddedTestServer> https_server_;

  brave_rewards::RewardsServiceImpl* rewards_service_;

  std::unique_ptr<base::RunLoop> wait_for_publisher_list_normalized_loop_;
  bool publisher_list_normalized_ = false;

  std::unique_ptr<base::RunLoop> wait_for_ac_completed_loop_;
  bool ac_reconcile_completed_ = false;
  ledger::Result ac_reconcile_status_ = ledger::Result::LEDGER_ERROR;
  std::unique_ptr<base::RunLoop> wait_for_tip_completed_loop_;

  std::unique_ptr<base::RunLoop> wait_for_multiple_ac_completed_loop_;
  bool multiple_ac_reconcile_completed_ = false;
  int32_t multiple_ac_reconcile_count_ = 0;
  int32_t multiple_ac_reconcile_needed_ = 0;
  std::vector<ledger::Result> multiple_ac_reconcile_status_;

  bool tip_reconcile_completed_ = false;
  ledger::Result tip_reconcile_status_ = ledger::Result::LEDGER_ERROR;

  std::unique_ptr<base::RunLoop> wait_for_multiple_tip_completed_loop_;
  bool multiple_tip_reconcile_completed_ = false;
  int32_t multiple_tip_reconcile_count_ = 0;
  int32_t multiple_tip_reconcile_needed_ = 0;
  ledger::Result multiple_tip_reconcile_status_ = ledger::Result::LEDGER_ERROR;

  std::unique_ptr<base::RunLoop> wait_for_recurring_tip_saved_loop_;
  bool recurring_tip_saved_ = false;

  std::unique_ptr<base::RunLoop> wait_for_pending_tip_saved_loop_;
  bool pending_tip_saved_ = false;

  std::unique_ptr<base::RunLoop> wait_for_attestation_loop_;

  bool last_publisher_added_ = false;
  bool show_defaults_in_properties_ = false;
  double balance_ = 0;
  double reconciled_tip_total_ = 0;
  double pending_balance_ = 0;
  double external_balance_ = 0;
  bool verified_wallet_ = false;
};

// #5 - Auto contribution
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, AutoContribution) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  // Visit verified publisher
  const bool verified = true;
  VisitPublisher("duckduckgo.com", verified);

  // Trigger contribution process
  rewards_service()->StartMonthlyContributionForTest();

  // Wait for reconciliation to complete successfully
  WaitForACReconcileCompleted();
  ASSERT_EQ(ac_reconcile_status_, ledger::Result::LEDGER_OK);

  // Make sure that balance is updated correctly
  IsBalanceCorrect();

  // Check that summary table shows the appropriate contribution
  rewards_browsertest_util::WaitForElementToContain(
      contents(),
      "[color=contribute]",
      "-20.000BAT");
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
    AutoContributionMultiplePublishers) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  // Visit verified publisher
  const bool verified = true;
  VisitPublisher("duckduckgo.com", verified);
  VisitPublisher("laurenwags.github.io", verified);

  // Trigger contribution process
  rewards_service()->StartMonthlyContributionForTest();

  // Wait for reconciliation to complete successfully
  WaitForACReconcileCompleted();
  ASSERT_EQ(ac_reconcile_status_, ledger::Result::LEDGER_OK);

  // Make sure that balance is updated correctly
  IsBalanceCorrect();

  // Check that summary table shows the appropriate contribution
  rewards_browsertest_util::WaitForElementToContain(
      contents(),
      "[color=contribute]",
      "-20.000BAT");
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
    AutoContributionMultiplePublishersUphold) {
  SetUpUpholdWallet(50.0);
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  ledger::SKUOrderItemList items;
  auto item = ledger::SKUOrderItem::New();
  item->order_item_id = "ed193339-e58c-483c-8d61-7decd3c24827";
  item->order_id = "a38b211b-bf78-42c8-9479-b11e92e3a76c";
  item->quantity = 80;
  item->price = 0.25;
  item->description = "description";
  item->type = ledger::SKUOrderItemType::SINGLE_USE;
  items.push_back(std::move(item));

  auto order = ledger::SKUOrder::New();
  order->order_id = "a38b211b-bf78-42c8-9479-b11e92e3a76c";
  order->total_amount = 20;
  order->merchant_id = "";
  order->location = "brave.com";
  order->items = std::move(items);
  response_->SetSKUOrder(std::move(order));

  // Visit verified publisher
  const bool verified = true;
  VisitPublisher("duckduckgo.com", verified);
  VisitPublisher("laurenwags.github.io", verified);

  // Trigger contribution process
  rewards_service()->StartMonthlyContributionForTest();

  // Wait for reconciliation to complete successfully
  WaitForACReconcileCompleted();
  ASSERT_EQ(ac_reconcile_status_, ledger::Result::LEDGER_OK);

  // Make sure that balance is updated correctly
  IsBalanceCorrect();

  // Check that summary table shows the appropriate contribution
  rewards_browsertest_util::WaitForElementToContain(
      contents(),
      "[color=contribute]",
      "-20.000BAT");
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, AutoContributeWhenACOff) {
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  // Visit verified publisher
  const bool verified = true;
  VisitPublisher("duckduckgo.com", verified);

  // toggle auto contribute off
  rewards_browsertest_util::WaitForElementThenClick(
      contents(),
      "[data-test-id2='autoContribution']");
  std::string value =
      rewards_browsertest_util::WaitForElementThenGetAttribute(
        contents(),
        "[data-test-id2='autoContribution']",
        "data-toggled");
  ASSERT_STREQ(value.c_str(), "false");

  // Trigger contribution process
  rewards_service()->StartMonthlyContributionForTest();
}

// #6 - Tip verified publisher
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, TipVerifiedPublisher) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  // Tip verified publisher
  TipPublisher(
      "duckduckgo.com",
      rewards_browsertest_util::ContributionType::OneTimeTip,
      true);
}

// #7 - Tip unverified publisher
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, TipUnverifiedPublisher) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  // Tip unverified publisher
  TipPublisher(
      "brave.com",
      rewards_browsertest_util::ContributionType::OneTimeTip);
}

// #8 - Recurring tip for verified publisher
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       RecurringTipForVerifiedPublisher) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  // Tip verified publisher
  TipPublisher(
      "duckduckgo.com",
      rewards_browsertest_util::ContributionType::MonthlyTip,
      true);
}

// #9 - Recurring tip for unverified publisher
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       RecurringTipForUnverifiedPublisher) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  // Tip verified publisher
  TipPublisher(
      "brave.com",
      rewards_browsertest_util::ContributionType::MonthlyTip,
      false);
}

// Brave tip icon is injected when visiting Twitter
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, TwitterTipsInjectedOnTwitter) {
  // Enable Rewards
  rewards_browsertest_util::EnableRewardsViaCode(browser(), rewards_service());

  // Navigate to Twitter in a new tab
  GURL url = https_server()->GetURL("twitter.com", "/twitter");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Ensure that Media tips injection is active
  rewards_browsertest_util::IsMediaTipsInjected(contents(), true);
}

// Brave tip icon is not injected when visiting Twitter while Brave
// Rewards is disabled
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       TwitterTipsNotInjectedWhenRewardsDisabled) {
  // Navigate to Twitter in a new tab
  GURL url = https_server()->GetURL("twitter.com", "/twitter");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Ensure that Media tips injection is not active
  rewards_browsertest_util::IsMediaTipsInjected(contents(), false);
}

// Brave tip icon is injected when visiting old Twitter
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       TwitterTipsInjectedOnOldTwitter) {
  // Enable Rewards
  rewards_browsertest_util::EnableRewardsViaCode(browser(), rewards_service());

  // Navigate to Twitter in a new tab
  GURL url = https_server()->GetURL("twitter.com", "/oldtwitter");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Ensure that Media tips injection is active
  rewards_browsertest_util::IsMediaTipsInjected(contents(), true);
}

// Brave tip icon is not injected when visiting old Twitter while
// Brave Rewards is disabled
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       TwitterTipsNotInjectedWhenRewardsDisabledOldTwitter) {
  // Navigate to Twitter in a new tab
  GURL url = https_server()->GetURL("twitter.com", "/oldtwitter");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Ensure that Media tips injection is not active
  rewards_browsertest_util::IsMediaTipsInjected(contents(), false);
}

// Brave tip icon is not injected into non-Twitter sites
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       TwitterTipsNotInjectedOnNonTwitter) {
  // Enable Rewards
  rewards_browsertest_util::EnableRewardsViaCode(browser(), rewards_service());

  // Navigate to a non-Twitter site in a new tab
  GURL url = https_server()->GetURL("brave.com", "/twitter");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Ensure that Media tips injection is not active
  rewards_browsertest_util::IsMediaTipsInjected(contents(), false);
}

// Brave tip icon is injected when visiting Reddit
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, RedditTipsInjectedOnReddit) {
  // Enable Rewards
  rewards_browsertest_util::EnableRewardsViaCode(browser(), rewards_service());

  // Navigate to Reddit in a new tab
  GURL url = https_server()->GetURL("reddit.com", "/reddit");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Ensure that Media Tips injection is active
  rewards_browsertest_util::IsMediaTipsInjected(contents(), true);
}

// Brave tip icon is not injected when visiting Reddit
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       RedditTipsNotInjectedWhenRewardsDisabled) {
  // Navigate to Reddit in a new tab
  GURL url = https_server()->GetURL("reddit.com", "/reddit");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Ensure that Media Tips injection is not active
  rewards_browsertest_util::IsMediaTipsInjected(contents(), false);
}

// Brave tip icon is not injected when visiting Reddit
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       RedditTipsNotInjectedOnNonReddit) {
  // Enable Rewards
  rewards_browsertest_util::EnableRewardsViaCode(browser(), rewards_service());

  // Navigate to Reddit in a new tab
  GURL url = https_server()->GetURL("brave.com", "/reddit");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Ensure that Media Tips injection is not active
  rewards_browsertest_util::IsMediaTipsInjected(contents(), false);
}

// Brave tip icon is injected when visiting GitHub
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, GitHubTipsInjectedOnGitHub) {
  // Enable Rewards
  rewards_browsertest_util::EnableRewardsViaCode(browser(), rewards_service());

  // Navigate to GitHub in a new tab
  GURL url = https_server()->GetURL("github.com", "/github");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Ensure that Media Tips injection is active
  rewards_browsertest_util::IsMediaTipsInjected(contents(), true);
}

// Brave tip icon is not injected when visiting GitHub while Brave
// Rewards is disabled
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       GitHubTipsNotInjectedWhenRewardsDisabled) {
  // Navigate to GitHub in a new tab
  GURL url = https_server()->GetURL("github.com", "/github");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Ensure that Media Tips injection is not active
  rewards_browsertest_util::IsMediaTipsInjected(contents(), false);
}

// Brave tip icon is not injected when not visiting GitHub
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       GitHubTipsNotInjectedOnNonGitHub) {
  // Enable Rewards
  rewards_browsertest_util::EnableRewardsViaCode(browser(), rewards_service());

  // Navigate to GitHub in a new tab
  GURL url = https_server()->GetURL("brave.com", "/github");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Ensure that Media Tips injection is not active
  rewards_browsertest_util::IsMediaTipsInjected(contents(), false);
}

// Check pending contributions
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       PendingContributionTip) {
  const std::string publisher = "example.com";

  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  // Tip unverified publisher
  TipPublisher(
      publisher,
      rewards_browsertest_util::ContributionType::OneTimeTip);

  // Check that link for pending is shown and open modal
  rewards_browsertest_util::WaitForElementThenClick(
      contents(),
      "[data-test-id='reservedAllLink']");

  // Make sure that table is populated
  rewards_browsertest_util::WaitForElementToContain(
      contents(),
      "[id='pendingContributionTable'] a",
      publisher);
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, ProcessPendingContributions) {
  response_->SetAlternativePublisherList(true);

  rewards_browsertest_helper::EnableRewards(browser());

  contents()->GetController().Reload(content::ReloadType::NORMAL, true);
  EXPECT_TRUE(WaitForLoadStop(contents()));

  // Tip unverified publisher
  TipViaCode("brave.com", 1.0, ledger::PublisherStatus::NOT_VERIFIED);
  TipViaCode("brave.com", 5.0, ledger::PublisherStatus::NOT_VERIFIED);
  TipViaCode("3zsistemi.si", 10.0, ledger::PublisherStatus::NOT_VERIFIED);
  TipViaCode("3zsistemi.si", 5.0, ledger::PublisherStatus::NOT_VERIFIED);
  TipViaCode("3zsistemi.si", 10.0, ledger::PublisherStatus::NOT_VERIFIED);
  TipViaCode("3zsistemi.si", 10.0, ledger::PublisherStatus::NOT_VERIFIED);

  balance_ = promotion_->ClaimPromotionViaCode();

  response_->SetAlternativePublisherList(false);
  VerifyTip(41.0, false, false, true);

  // Visit publisher
  GURL url = https_server()->GetURL("3zsistemi.si", "/index.html");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Refresh publisher list
  RefreshPublisherListUsingRewardsPopup();

  // Activate the Rewards settings page tab
  ActivateTabAtIndex(0);

  // Wait for new verified publisher to be processed
  WaitForMultipleTipReconcileCompleted(3);
  ASSERT_EQ(multiple_tip_reconcile_status_, ledger::Result::LEDGER_OK);
  UpdateContributionBalance(-25.0, false);  // update pending balance

  // Make sure that balance is updated correctly
  IsBalanceCorrect();

  // Check that wallet summary shows the appropriate tip amount
  rewards_browsertest_util::WaitForElementToEqual(
      contents(),
      "[data-test-id=summary-tips] [color=contribute] span span",
      ExpectedTipSummaryAmountString());

  // Make sure that pending contribution box shows the correct
  // amount
  IsPendingBalanceCorrect();

  // Open the Rewards popup
  content::WebContents* popup_contents =
      rewards_browsertest_helper::OpenRewardsPopup(browser());
  ASSERT_TRUE(popup_contents);

  // Check if insufficient funds notification is shown
  rewards_browsertest_util::WaitForElementToContain(
      popup_contents,
      "#root",
      "Insufficient Funds");

  // Close notification
  rewards_browsertest_util::WaitForElementThenClick(
      popup_contents,
      "[data-test-id=notification-close]");

  // Check if verified notification is shown
  rewards_browsertest_util::WaitForElementToContain(
      popup_contents,
      "#root",
      "3zsistemi.si");
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       TipWithVerifiedWallet) {
  SetUpUpholdWallet(50.0);

  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  const double amount = 5.0;
  const bool should_contribute = true;
  TipViaCode(
      "duckduckgo.com",
      amount,
      ledger::PublisherStatus::VERIFIED,
      should_contribute);
  VerifyTip(amount, should_contribute, false, true);
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       MultipleTipsProduceMultipleFeesWithVerifiedWallet) {
  SetUpUpholdWallet(50.0);

  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  double total_amount = 0.0;
  const double amount = 5.0;
  const double fee_percentage = 0.05;
  const double tip_fee = amount * fee_percentage;
  const bool should_contribute = true;
  TipViaCode(
      "duckduckgo.com",
      amount,
      ledger::PublisherStatus::VERIFIED,
      should_contribute);
  total_amount += amount;

  TipViaCode(
      "laurenwags.github.io",
      amount,
      ledger::PublisherStatus::VERIFIED,
      should_contribute);
  total_amount += amount;

  VerifyTip(total_amount, should_contribute, false, true);

  ledger::TransferFeeList transfer_fees =
      rewards_service()->GetTransferFeesForTesting("uphold");

  ASSERT_EQ(transfer_fees.size(), 2UL);

  for (auto const& value : transfer_fees) {
    ASSERT_EQ(value.second->amount, tip_fee);
  }
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, TipConnectedPublisherAnon) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  // Tip verified publisher
  const double amount = 5.0;
  const bool should_contribute = true;
  TipViaCode(
      "bumpsmack.com",
      amount,
      ledger::PublisherStatus::CONNECTED,
      should_contribute);
  VerifyTip(amount, should_contribute, false, true);
}

IN_PROC_BROWSER_TEST_F(
    RewardsBrowserTest,
    TipConnectedPublisherAnonAndConnected) {
  SetUpUpholdWallet(50.0);

  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  // Tip verified publisher
  const double amount = 5.0;
  const bool should_contribute = true;
  TipViaCode(
      "bumpsmack.com",
      amount,
      ledger::PublisherStatus::CONNECTED,
      should_contribute);
  VerifyTip(amount, should_contribute, false, true);
}

IN_PROC_BROWSER_TEST_F(
    RewardsBrowserTest,
    TipConnectedPublisherConnected) {
  SetUpUpholdWallet(50.0, ledger::WalletStatus::CONNECTED);

  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());
  contents()->GetController().Reload(content::ReloadType::NORMAL, true);
  EXPECT_TRUE(WaitForLoadStop(contents()));

  // Tip connected publisher
  const double amount = 5.0;
  const bool should_contribute = false;
  TipViaCode(
      "bumpsmack.com",
      amount,
      ledger::PublisherStatus::CONNECTED,
      should_contribute,
      false,
      ledger::Result::LEDGER_ERROR);

  IsBalanceCorrect();

  // Make sure that tips table is empty
  rewards_browsertest_util::WaitForElementToEqual(
      contents(),
      "#tips-table > div > div",
      "Have you tipped your favorite content creator today?");
}

IN_PROC_BROWSER_TEST_F(
    RewardsBrowserTest,
    TipConnectedPublisherVerified) {
  SetUpUpholdWallet(50.0);

  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());
  contents()->GetController().Reload(content::ReloadType::NORMAL, true);
  EXPECT_TRUE(WaitForLoadStop(contents()));

  // Tip connected publisher
  const double amount = 5.0;
  const bool should_contribute = false;
  TipViaCode(
      "bumpsmack.com",
      amount,
      ledger::PublisherStatus::CONNECTED,
      should_contribute,
      false,
      ledger::Result::LEDGER_ERROR);

  IsBalanceCorrect();

  // Make sure that tips table is empty
  rewards_browsertest_util::WaitForElementToEqual(
      contents(),
      "#tips-table > div > div",
      "Have you tipped your favorite content creator today?");
}

// Ensure that we can make a one-time tip of a non-integral amount.
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, TipNonIntegralAmount) {
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  // TODO(jhoneycutt): Test that this works through the tipping UI.
  rewards_service()->OnTip("duckduckgo.com", 2.5, false);
  WaitForTipReconcileCompleted();
  ASSERT_EQ(tip_reconcile_status_, ledger::Result::LEDGER_OK);

  ASSERT_EQ(reconciled_tip_total_, 2.5);
}

// Ensure that we can make a recurring tip of a non-integral amount.
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, RecurringTipNonIntegralAmount) {
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  const bool verified = true;
  VisitPublisher("duckduckgo.com", verified);

  rewards_service()->OnTip("duckduckgo.com", 2.5, true);
  rewards_service()->StartMonthlyContributionForTest();
  WaitForTipReconcileCompleted();
  ASSERT_EQ(tip_reconcile_status_, ledger::Result::LEDGER_OK);

  ASSERT_EQ(reconciled_tip_total_, 2.5);
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
    RecurringAndPartialAutoContribution) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  // Visit verified publisher
  const bool verified = true;
  VisitPublisher("duckduckgo.com", verified);

  // Set monthly recurring
  TipViaCode(
      "duckduckgo.com",
      25.0,
      ledger::PublisherStatus::VERIFIED,
      false,
      true);

  VisitPublisher("brave.com", !verified);

  // Trigger contribution process
  rewards_service()->StartMonthlyContributionForTest();

  // Wait for reconciliation to complete
  WaitForTipReconcileCompleted();
  ASSERT_EQ(tip_reconcile_status_, ledger::Result::LEDGER_OK);

  // Wait for reconciliation to complete successfully
  WaitForACReconcileCompleted();
  ASSERT_EQ(ac_reconcile_status_, ledger::Result::LEDGER_OK);

  // Make sure that balance is updated correctly
  IsBalanceCorrect();

  // Check that summary table shows the appropriate contribution
  rewards_browsertest_util::WaitForElementToContain(
      contents(),
      "[color='contribute']",
      "-5.000BAT");
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
    MultipleRecurringOverBudgetAndPartialAutoContribution) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  TipViaCode(
      "duckduckgo.com",
      5.0,
      ledger::PublisherStatus::VERIFIED,
      false,
      true);

  TipViaCode(
      "site1.com",
      10.0,
      ledger::PublisherStatus::VERIFIED,
      false,
      true);

  TipViaCode(
      "site2.com",
      10.0,
      ledger::PublisherStatus::VERIFIED,
      false,
      true);

  TipViaCode(
      "site3.com",
      10.0,
      ledger::PublisherStatus::VERIFIED,
      false,
      true);

  balance_ = promotion_->ClaimPromotionViaCode();

  // Visit verified publisher
  const bool verified = true;
  VisitPublisher("duckduckgo.com", verified);

  // Trigger contribution process
  rewards_service()->StartMonthlyContributionForTest();

  // Wait for reconciliation to complete
  WaitForMultipleTipReconcileCompleted(3);
  ASSERT_EQ(tip_reconcile_status_, ledger::Result::LEDGER_OK);

  // Wait for reconciliation to complete successfully
  WaitForACReconcileCompleted();
  ASSERT_EQ(ac_reconcile_status_, ledger::Result::LEDGER_OK);

  // Make sure that balance is updated correctly
  IsBalanceCorrect();

  // Check that summary table shows the appropriate contribution

  // Check that summary table shows the appropriate contribution
  rewards_browsertest_util::WaitForElementToContain(
      contents(),
      "[color='contribute']",
      "-5.000BAT");
}

IN_PROC_BROWSER_TEST_F(
    RewardsBrowserTest,
    SplitProcessorAutoContribution) {
  SetUpUpholdWallet(50.0);

  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  VisitPublisher("3zsistemi.si", true);

  // 30 form unblinded and 20 from uphold
  rewards_service()->SetAutoContributionAmount(50.0);

  ledger::SKUOrderItemList items;
  auto item = ledger::SKUOrderItem::New();
  item->order_item_id = "ed193339-e58c-483c-8d61-7decd3c24827";
  item->order_id = "a38b211b-bf78-42c8-9479-b11e92e3a76c";
  item->quantity = 80;
  item->price = 0.25;
  item->description = "description";
  item->type = ledger::SKUOrderItemType::SINGLE_USE;
  items.push_back(std::move(item));

  auto order = ledger::SKUOrder::New();
  order->order_id = "a38b211b-bf78-42c8-9479-b11e92e3a76c";
  order->total_amount = 20;
  order->merchant_id = "";
  order->location = "brave.com";
  order->items = std::move(items);
  response_->SetSKUOrder(std::move(order));

  // Trigger contribution process
  rewards_service()->StartMonthlyContributionForTest();

  // Wait for reconciliation to complete successfully
  WaitForMultipleACReconcileCompleted(2);
  ASSERT_EQ(multiple_ac_reconcile_status_[0], ledger::Result::LEDGER_OK);
  ASSERT_EQ(multiple_ac_reconcile_status_[1], ledger::Result::LEDGER_OK);

  rewards_browsertest_util::WaitForElementThenClick(
      contents(),
      "[data-test-id='showMonthlyReport']");

  rewards_browsertest_util::WaitForElementToAppear(
      contents(),
      "#transactionTable");

  rewards_browsertest_util::WaitForElementToContain(
      contents(),
      "#transactionTable",
      "-30.000BAT");

  rewards_browsertest_util::WaitForElementToContain(
      contents(),
      "#transactionTable",
      "-20.000BAT");

  // Check that summary table shows the appropriate contribution
  rewards_browsertest_util::WaitForElementToContain(
      contents(),
      "[color=contribute]",
      "-50.000BAT");
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, CheckIfReconcileWasReset) {
  rewards_browsertest_helper::EnableRewards(browser());
  uint64_t current_stamp = 0;

  base::RunLoop run_loop_first;
  rewards_service_->GetReconcileStamp(
      base::BindLambdaForTesting([&](uint64_t stamp) {
        current_stamp = stamp;
        run_loop_first.Quit();
      }));
  run_loop_first.Run();

  balance_ = promotion_->ClaimPromotionViaCode();

  VisitPublisher("duckduckgo.com", true);

  TipPublisher(
      "duckduckgo.com",
      rewards_browsertest_util::ContributionType::MonthlyTip,
      true);

  base::RunLoop run_loop_second;
  rewards_service_->GetReconcileStamp(
      base::BindLambdaForTesting([&](uint64_t stamp) {
        ASSERT_NE(current_stamp, stamp);
        run_loop_second.Quit();
      }));
  run_loop_second.Run();
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, CheckIfReconcileWasResetACOff) {
  rewards_browsertest_helper::EnableRewards(browser());
  uint64_t current_stamp = 0;

  rewards_service_->SetAutoContributeEnabled(false);

  base::RunLoop run_loop_first;
  rewards_service_->GetReconcileStamp(
      base::BindLambdaForTesting([&](uint64_t stamp) {
        current_stamp = stamp;
        run_loop_first.Quit();
      }));
  run_loop_first.Run();

  balance_ = promotion_->ClaimPromotionViaCode();
  TipPublisher(
      "duckduckgo.com",
      rewards_browsertest_util::ContributionType::MonthlyTip,
      true);

  base::RunLoop run_loop_second;
  rewards_service_->GetReconcileStamp(
      base::BindLambdaForTesting([&](uint64_t stamp) {
        ASSERT_NE(current_stamp, stamp);
        run_loop_second.Quit();
      }));
  run_loop_second.Run();
}

IN_PROC_BROWSER_TEST_F(
    RewardsBrowserTest,
    SplitProcessOneTimeTip) {
  SetUpUpholdWallet(50.0);
  rewards_browsertest_helper::EnableRewards(browser());
  balance_ = promotion_->ClaimPromotionViaCode();

  TipPublisher(
      "kjozwiakstaging.github.io",
      rewards_browsertest_util::ContributionType::OneTimeTip,
      true,
      1,
      2);

  ActivateTabAtIndex(0);

  rewards_browsertest_util::WaitForElementThenClick(
      contents(),
      "[data-test-id='showMonthlyReport']");

  rewards_browsertest_util::WaitForElementThenClick(
      contents(),
      "[data-test-id='tab-oneTimeDonation']");

  rewards_browsertest_util::WaitForElementToEqual(
      contents(),
      "[data-test-id='activity-table-body'] tr:nth-of-type(1) "
      "td:nth-of-type(3)",
      "20.000BAT28.60 USD");

  rewards_browsertest_util::WaitForElementToEqual(
      contents(),
      "[data-test-id='activity-table-body'] tr:nth-of-type(2) "
      "td:nth-of-type(3)",
      "30.000BAT42.90 USD");
}

}  // namespace rewards_browsertest
