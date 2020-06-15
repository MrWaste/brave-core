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
    if (!rewards_service_->IsWalletInitialized()) {
      WaitForWalletInitialization();
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

  void WaitForWalletInitialization() {
    if (wallet_initialized_)
      return;
    wait_for_wallet_initialization_loop_.reset(new base::RunLoop);
    wait_for_wallet_initialization_loop_->Run();
  }

  void WaitForPublisherListNormalized() {
    if (publisher_list_normalized_)
      return;
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
    if (multiple_tip_reconcile_completed_ ||
        multiple_tip_reconcile_count_ == needed) {
      return;
    }

    wait_for_multiple_tip_completed_loop_.reset(new base::RunLoop);
    wait_for_multiple_tip_completed_loop_->Run();
  }

  void WaitForMultipleACReconcileCompleted(int32_t needed) {
    multiple_ac_reconcile_needed_ = needed;
    if (multiple_ac_reconcile_completed_) {
      return;
    }

    wait_for_multiple_ac_completed_loop_.reset(new base::RunLoop);
    wait_for_multiple_ac_completed_loop_->Run();
  }

  void WaitForInsufficientFundsNotification() {
    if (insufficient_notification_would_have_already_shown_) {
      return;
    }
    wait_for_insufficient_notification_loop_.reset(new base::RunLoop);
    wait_for_insufficient_notification_loop_->Run();
  }

  void WaitForRecurringTipToBeSaved() {
    if (recurring_tip_saved_) {
      return;
    }
    wait_for_recurring_tip_saved_loop_.reset(new base::RunLoop);
    wait_for_recurring_tip_saved_loop_->Run();
  }

  void AddNotificationServiceObserver() {
    rewards_service_->GetNotificationService()->AddObserver(this);
  }

  bool IsShowingNotificationForType(
      const RewardsNotificationType type) const {
    const auto& notifications = rewards_service_->GetAllNotifications();
    for (const auto& notification : notifications) {
      if (notification.second.type_ == type) {
        return true;
      }
    }

    return false;
  }

  void GetReconcileInterval() {
    rewards_service()->GetReconcileInterval(
        base::Bind(&RewardsBrowserTest::OnGetReconcileInterval,
          base::Unretained(this)));
  }

  void GetShortRetries() {
    rewards_service()->GetShortRetries(
        base::Bind(&RewardsBrowserTest::OnGetShortRetries,
          base::Unretained(this)));
  }

  void GetEnvironment() {
    rewards_service()->GetEnvironment(
        base::Bind(&RewardsBrowserTest::OnGetEnvironment,
          base::Unretained(this)));
  }

  void GetDebug() {
    rewards_service()->GetDebug(
        base::Bind(&RewardsBrowserTest::OnGetDebug,
          base::Unretained(this)));
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

  GURL uphold_auth_url() {
    GURL url("chrome://rewards/uphold/authorization?"
             "code=0c42b34121f624593ee3b04cbe4cc6ddcd72d&state=123456789");
    return url;
  }

  content::WebContents* contents() const {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  brave_rewards::RewardsServiceImpl* rewards_service() {
    return rewards_service_;
  }

  void VisitPublisher(const std::string& publisher,
                      bool verified,
                      bool last_add = false) {
    GURL url = https_server()->GetURL(publisher, "/index.html");
    ui_test_utils::NavigateToURLWithDisposition(
        browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

    // The minimum publisher duration when testing is 1 second (and the
    // granularity is seconds), so wait for just over 2 seconds to elapse
    base::PlatformThread::Sleep(base::TimeDelta::FromMilliseconds(2100));

    // Activate the Rewards settings page tab
    ActivateTabAtIndex(0);

    // Wait for publisher list normalization
    WaitForPublisherListNormalized();

    // Make sure site appears in auto-contribute table
    rewards_browsertest_util::WaitForElementToEqual(
        contents(),
        "[data-test-id='ac_link_" + publisher + "']",
        publisher);

    if (verified) {
      // A verified site has two images associated with it, the site's
      // favicon and the verified icon
      content::EvalJsResult js_result =
          EvalJs(contents(),
                "document.querySelector(\"[data-test-id='ac_link_" +
                publisher + "']\").getElementsByTagName('svg').length === 1;",
                content::EXECUTE_SCRIPT_DEFAULT_OPTIONS,
                content::ISOLATED_WORLD_ID_CONTENT_END);
      EXPECT_TRUE(js_result.ExtractBool());
    } else {
      // An unverified site has one image associated with it, the site's
      // favicon
      content::EvalJsResult js_result =
          EvalJs(contents(),
                "document.querySelector(\"[data-test-id='ac_link_" +
                publisher + "']\").getElementsByTagName('svg').length === 0;",
                content::EXECUTE_SCRIPT_DEFAULT_OPTIONS,
                content::ISOLATED_WORLD_ID_CONTENT_END);
      EXPECT_TRUE(js_result.ExtractBool());
    }

    if (last_add) {
      last_publisher_added_ = true;
    }
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

  void ActivateTabAtIndex(int index) const {
    browser()->tab_strip_model()->ActivateTabAt(
        index,
        TabStripModel::UserGestureDetails(TabStripModel::GestureType::kOther));
  }

  void RefreshPublisherListUsingRewardsPopup() const {
    rewards_browsertest_util::WaitForElementThenClick(
        rewards_browsertest_helper::OpenRewardsPopup(browser()),
        "[data-test-id='unverified-check-button']");
  }

  content::WebContents* OpenSiteBanner(
      rewards_browsertest_util::ContributionType banner_type) const {
    content::WebContents* popup_contents =
        rewards_browsertest_helper::OpenRewardsPopup(browser());

    // Construct an observer to wait for the site banner to load.
    content::WindowedNotificationObserver site_banner_observer(
        content::NOTIFICATION_LOAD_COMPLETED_MAIN_FRAME,
        content::NotificationService::AllSources());

    const std::string buttonSelector =
        banner_type == rewards_browsertest_util::ContributionType::MonthlyTip
        ? "[type='tip-monthly']"
        : "[type='tip']";

    // Click button to initiate sending a tip.
    rewards_browsertest_util::WaitForElementThenClick(
        popup_contents,
        buttonSelector);

    // Wait for the site banner to load
    site_banner_observer.Wait();

    // Retrieve the notification source
    const auto& site_banner_source =
        static_cast<const content::Source<content::WebContents>&>(
            site_banner_observer.source());

    // Allow the site banner to update its UI. We cannot use ExecJs here,
    // because it does not resolve promises.
    (void)EvalJs(site_banner_source.ptr(),
        "new Promise(resolve => setTimeout(resolve, 0))",
        content::EXECUTE_SCRIPT_DEFAULT_OPTIONS,
        content::ISOLATED_WORLD_ID_CONTENT_END);

    return site_banner_source.ptr();
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

    content::WebContents* site_banner_contents = OpenSiteBanner(type);
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

  void OnWalletInitialized(brave_rewards::RewardsService* rewards_service,
                           int32_t result) {
    const auto converted_result = static_cast<ledger::Result>(result);
    ASSERT_TRUE(converted_result == ledger::Result::WALLET_CREATED ||
                converted_result == ledger::Result::LEDGER_OK);
    wallet_initialized_ = true;
    if (wait_for_wallet_initialization_loop_)
      wait_for_wallet_initialization_loop_->Quit();
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

  void OnNotificationAdded(
      brave_rewards::RewardsNotificationService* rewards_notification_service,
      const brave_rewards::RewardsNotificationService::RewardsNotification&
      notification) {
    const auto& notifications =
        rewards_notification_service->GetAllNotifications();

    for (const auto& notification : notifications) {
      switch (notification.second.type_) {
        case brave_rewards::RewardsNotificationService::
            RewardsNotificationType::REWARDS_NOTIFICATION_INSUFFICIENT_FUNDS: {
          insufficient_notification_would_have_already_shown_ = true;
          if (wait_for_insufficient_notification_loop_) {
            wait_for_insufficient_notification_loop_->Quit();
          }

          break;
        }
        default: {
          break;
        }
      }
    }
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

  void CheckInsufficientFundsForTesting() {
    rewards_service_->MaybeShowNotificationAddFundsForTesting(
        base::BindOnce(
            &RewardsBrowserTest::ShowNotificationAddFundsForTesting,
            AsWeakPtr()));
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

  MOCK_METHOD1(OnGetEnvironment, void(ledger::Environment));
  MOCK_METHOD1(OnGetDebug, void(bool));
  MOCK_METHOD1(OnGetReconcileInterval, void(int32_t));
  MOCK_METHOD1(OnGetShortRetries, void(bool));

  std::unique_ptr<RewardsBrowserTestResponse> response_;
  std::unique_ptr<RewardsBrowserTestPromotion> promotion_;

  std::unique_ptr<net::EmbeddedTestServer> https_server_;

  brave_rewards::RewardsServiceImpl* rewards_service_;

  std::unique_ptr<base::RunLoop> wait_for_wallet_initialization_loop_;
  bool wallet_initialized_ = false;

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

  std::unique_ptr<base::RunLoop> wait_for_insufficient_notification_loop_;
  bool insufficient_notification_would_have_already_shown_ = false;

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

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, RenderWelcome) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());
  EXPECT_STREQ(contents()->GetLastCommittedURL().spec().c_str(),
      // actual url is always chrome://
      "chrome://rewards/");
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, ToggleRewards) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  // Toggle rewards off
  rewards_browsertest_util::WaitForElementThenClick(
      contents(),
      "[data-test-id2='enableMain']");
  std::string value =
      rewards_browsertest_util::WaitForElementThenGetAttribute(
        contents(),
        "[data-test-id2='enableMain']",
        "data-toggled");
  ASSERT_STREQ(value.c_str(), "false");

  // Toggle rewards back on
  rewards_browsertest_util::WaitForElementThenClick(
      contents(),
      "[data-test-id2='enableMain']");
  value = rewards_browsertest_util::WaitForElementThenGetAttribute(
      contents(),
      "[data-test-id2='enableMain']",
      "data-toggled");
  ASSERT_STREQ(value.c_str(), "true");
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, ToggleAutoContribute) {
  rewards_browsertest_helper::EnableRewards(browser());

  // once rewards has loaded, reload page to activate auto-contribute
  contents()->GetController().Reload(content::ReloadType::NORMAL, true);
  EXPECT_TRUE(WaitForLoadStop(contents()));

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

  // toggle auto contribute back on
  rewards_browsertest_util::WaitForElementThenClick(
      contents(),
      "[data-test-id2='autoContribution']");
  value = rewards_browsertest_util::WaitForElementThenGetAttribute(
      contents(),
      "[data-test-id2='autoContribution']",
      "data-toggled");
  ASSERT_STREQ(value.c_str(), "true");
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, ActivateSettingsModal) {
  rewards_browsertest_helper::EnableRewards(browser());

  rewards_browsertest_util::WaitForElementThenClick(
      contents(),
      "[data-test-id='settingsButton']");
  rewards_browsertest_util::WaitForElementToAppear(
      contents(),
      "#modal");
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, HandleFlagsSingleArg) {
  testing::InSequence s;
  // SetEnvironment(ledger::Environment::PRODUCTION)
  EXPECT_CALL(*this, OnGetEnvironment(ledger::Environment::PRODUCTION));
  // Staging - true and 1
  EXPECT_CALL(*this, OnGetEnvironment(ledger::Environment::STAGING)).Times(2);
  // Staging - false and random
  EXPECT_CALL(*this, OnGetEnvironment(
      ledger::Environment::PRODUCTION)).Times(2);

  rewards_service()->SetEnvironment(ledger::Environment::PRODUCTION);
  GetEnvironment();
  rewards_browsertest_util::RunUntilIdle();

  // Staging - true
  rewards_service()->SetEnvironment(ledger::Environment::PRODUCTION);
  rewards_service()->HandleFlags("staging=true");
  GetEnvironment();
  rewards_browsertest_util::RunUntilIdle();

  // Staging - 1
  rewards_service()->SetEnvironment(ledger::Environment::PRODUCTION);
  rewards_service()->HandleFlags("staging=1");
  GetEnvironment();
  rewards_browsertest_util::RunUntilIdle();

  // Staging - false
  rewards_service()->SetEnvironment(ledger::Environment::STAGING);
  rewards_service()->HandleFlags("staging=false");
  GetEnvironment();
  rewards_browsertest_util::RunUntilIdle();

  // Staging - random
  rewards_service()->SetEnvironment(ledger::Environment::STAGING);
  rewards_service()->HandleFlags("staging=werwe");
  GetEnvironment();
  rewards_browsertest_util::RunUntilIdle();

  // SetDebug(true)
  EXPECT_CALL(*this, OnGetDebug(true));
  // Debug - true and 1
  EXPECT_CALL(*this, OnGetDebug(true)).Times(2);
  // Debug - false and random
  EXPECT_CALL(*this, OnGetDebug(false)).Times(2);

  rewards_service()->SetDebug(true);
  GetDebug();
  rewards_browsertest_util::RunUntilIdle();

  // Debug - true
  rewards_service()->SetDebug(false);
  rewards_service()->HandleFlags("debug=true");
  GetDebug();
  rewards_browsertest_util::RunUntilIdle();

  // Debug - 1
  rewards_service()->SetDebug(false);
  rewards_service()->HandleFlags("debug=1");
  GetDebug();
  rewards_browsertest_util::RunUntilIdle();

  // Debug - false
  rewards_service()->SetDebug(true);
  rewards_service()->HandleFlags("debug=false");
  GetDebug();
  rewards_browsertest_util::RunUntilIdle();

  // Debug - random
  rewards_service()->SetDebug(true);
  rewards_service()->HandleFlags("debug=werwe");
  GetDebug();
  rewards_browsertest_util::RunUntilIdle();

  // SetEnvironment(ledger::Environment::PRODUCTION)
  EXPECT_CALL(*this, OnGetEnvironment(ledger::Environment::PRODUCTION));
  // Development - true and 1
  EXPECT_CALL(
      *this,
      OnGetEnvironment(ledger::Environment::DEVELOPMENT)).Times(2);
  // Development - false and random
  EXPECT_CALL(
      *this,
      OnGetEnvironment(ledger::Environment::PRODUCTION)).Times(2);

  rewards_service()->SetEnvironment(ledger::Environment::PRODUCTION);
  GetEnvironment();
  rewards_browsertest_util::RunUntilIdle();

  // Development - true
  rewards_service()->SetEnvironment(ledger::Environment::PRODUCTION);
  rewards_service()->HandleFlags("development=true");
  GetEnvironment();
  rewards_browsertest_util::RunUntilIdle();

  // Development - 1
  rewards_service()->SetEnvironment(ledger::Environment::PRODUCTION);
  rewards_service()->HandleFlags("development=1");
  GetEnvironment();
  rewards_browsertest_util::RunUntilIdle();

  // Development - false
  rewards_service()->SetEnvironment(ledger::Environment::PRODUCTION);
  rewards_service()->HandleFlags("development=false");
  GetEnvironment();
  rewards_browsertest_util::RunUntilIdle();

  // Development - random
  rewards_service()->SetEnvironment(ledger::Environment::PRODUCTION);
  rewards_service()->HandleFlags("development=werwe");
  GetEnvironment();
  rewards_browsertest_util::RunUntilIdle();

  // positive number
  EXPECT_CALL(*this, OnGetReconcileInterval(10));
  // negative number and string
  EXPECT_CALL(*this, OnGetReconcileInterval(0)).Times(2);

  // Reconcile interval - positive number
  rewards_service()->SetReconcileInterval(0);
  rewards_service()->HandleFlags("reconcile-interval=10");
  GetReconcileInterval();
  rewards_browsertest_util::RunUntilIdle();

  // Reconcile interval - negative number
  rewards_service()->SetReconcileInterval(0);
  rewards_service()->HandleFlags("reconcile-interval=-1");
  GetReconcileInterval();
  rewards_browsertest_util::RunUntilIdle();

  // Reconcile interval - string
  rewards_service()->SetReconcileInterval(0);
  rewards_service()->HandleFlags("reconcile-interval=sdf");
  GetReconcileInterval();
  rewards_browsertest_util::RunUntilIdle();

  EXPECT_CALL(*this, OnGetShortRetries(true));   // on
  EXPECT_CALL(*this, OnGetShortRetries(false));  // off

  // Short retries - on
  rewards_service()->SetShortRetries(false);
  rewards_service()->HandleFlags("short-retries=true");
  GetShortRetries();
  rewards_browsertest_util::RunUntilIdle();

  // Short retries - off
  rewards_service()->SetShortRetries(true);
  rewards_service()->HandleFlags("short-retries=false");
  GetShortRetries();
  rewards_browsertest_util::RunUntilIdle();
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, HandleFlagsMultipleFlags) {
  EXPECT_CALL(*this, OnGetEnvironment(ledger::Environment::STAGING));
  EXPECT_CALL(*this, OnGetDebug(true));
  EXPECT_CALL(*this, OnGetReconcileInterval(10));
  EXPECT_CALL(*this, OnGetShortRetries(true));

  rewards_service()->SetEnvironment(ledger::Environment::PRODUCTION);
  rewards_service()->SetDebug(true);
  rewards_service()->SetReconcileInterval(0);
  rewards_service()->SetShortRetries(false);

  rewards_service()->HandleFlags(
      "staging=true,debug=true,short-retries=true,reconcile-interval=10");

  GetReconcileInterval();
  GetShortRetries();
  GetEnvironment();
  GetDebug();
  rewards_browsertest_util::RunUntilIdle();
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, HandleFlagsWrongInput) {
  EXPECT_CALL(*this, OnGetEnvironment(ledger::Environment::PRODUCTION));
  EXPECT_CALL(*this, OnGetDebug(false));
  EXPECT_CALL(*this, OnGetReconcileInterval(0));
  EXPECT_CALL(*this, OnGetShortRetries(false));

  rewards_service()->SetEnvironment(ledger::Environment::PRODUCTION);
  rewards_service()->SetDebug(false);
  rewards_service()->SetReconcileInterval(0);
  rewards_service()->SetShortRetries(false);

  rewards_service()->HandleFlags(
      "staging=,debug=,shortretries=true,reconcile-interval");

  GetReconcileInterval();
  GetShortRetries();
  GetDebug();
  GetEnvironment();
  rewards_browsertest_util::RunUntilIdle();
}

// #1 - Claim promotion via settings page
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, ClaimPromotionViaSettingsPage) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  // Claim and verify promotion using settings page
  const bool use_panel = false;
  balance_ = promotion_->ClaimPromotion(use_panel);
}

// #2 - Claim promotion via panel
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, ClaimPromotionViaPanel) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  // Claim and verify promotion using panel
  const bool use_panel = true;
  balance_ = promotion_->ClaimPromotion(use_panel);
}

// #3 - Panel shows correct publisher data
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       PanelShowsCorrectPublisherData) {
  // Enable Rewards
  rewards_browsertest_util::EnableRewardsViaCode(browser(), rewards_service());

  // Navigate to a verified site in a new tab
  const std::string publisher = "duckduckgo.com";
  GURL url = https_server()->GetURL(publisher, "/index.html");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
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

// #4a - Visit verified publisher
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, VisitVerifiedPublisher) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  // Visit verified publisher
  const bool verified = true;
  VisitPublisher("duckduckgo.com", verified);
}

// #4b - Visit unverified publisher
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, VisitUnverifiedPublisher) {
  // Enable Rewards
  rewards_browsertest_helper::EnableRewards(browser());

  // Visit unverified publisher
  const bool verified = false;
  VisitPublisher("brave.com", verified);
}

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

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
    InsufficientNotificationForZeroAmountZeroPublishers) {
  AddNotificationServiceObserver();
  rewards_browsertest_util::EnableRewardsViaCode(browser(), rewards_service());
  CheckInsufficientFundsForTesting();
  WaitForInsufficientFundsNotification();
  const brave_rewards::RewardsNotificationService::RewardsNotificationsMap&
      notifications = rewards_service_->GetAllNotifications();

  if (notifications.empty()) {
    SUCCEED();
    return;
  }

  bool is_showing_notification = IsShowingNotificationForType(
      RewardsNotificationType::REWARDS_NOTIFICATION_INSUFFICIENT_FUNDS);

  EXPECT_FALSE(is_showing_notification);
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       InsufficientNotificationForACNotEnoughFunds) {
  AddNotificationServiceObserver();
  rewards_browsertest_helper::EnableRewards(browser());

  // Visit publishers
  const bool verified = true;
  while (!last_publisher_added_) {
    VisitPublisher("duckduckgo.com", verified);
    VisitPublisher("bumpsmack.com", verified);
    VisitPublisher("brave.com", !verified, true);
  }

  CheckInsufficientFundsForTesting();
  WaitForInsufficientFundsNotification();
  const brave_rewards::RewardsNotificationService::RewardsNotificationsMap&
      notifications = rewards_service_->GetAllNotifications();

  if (notifications.empty()) {
    SUCCEED();
    return;
  }

  bool is_showing_notification = IsShowingNotificationForType(
      RewardsNotificationType::REWARDS_NOTIFICATION_INSUFFICIENT_FUNDS);

  EXPECT_FALSE(is_showing_notification);
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       InsufficientNotificationForInsufficientAmount) {
  AddNotificationServiceObserver();
  rewards_browsertest_helper::EnableRewards(browser());
  balance_ = promotion_->ClaimPromotionViaCode();

  TipViaCode(
      "duckduckgo.com",
      20.0,
      ledger::PublisherStatus::VERIFIED,
      false,
      true);

  TipViaCode(
      "brave.com",
      50.0,
      ledger::PublisherStatus::NOT_VERIFIED,
      false,
      true);

  CheckInsufficientFundsForTesting();
  WaitForInsufficientFundsNotification();
  const brave_rewards::RewardsNotificationService::RewardsNotificationsMap&
      notifications = rewards_service_->GetAllNotifications();

  if (notifications.empty()) {
    SUCCEED();
    return;
  }

  bool is_showing_notification = IsShowingNotificationForType(
      RewardsNotificationType::REWARDS_NOTIFICATION_INSUFFICIENT_FUNDS);

  EXPECT_FALSE(is_showing_notification);
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest,
                       InsufficientNotificationForVerifiedInsufficientAmount) {
  AddNotificationServiceObserver();
  rewards_browsertest_helper::EnableRewards(browser());
  balance_ = promotion_->ClaimPromotionViaCode();

  TipViaCode(
      "duckduckgo.com",
      50.0,
      ledger::PublisherStatus::VERIFIED,
      false,
      true);

  TipViaCode(
      "brave.com",
      50.0,
      ledger::PublisherStatus::NOT_VERIFIED,
      false,
      true);

  CheckInsufficientFundsForTesting();
  WaitForInsufficientFundsNotification();
  const brave_rewards::RewardsNotificationService::RewardsNotificationsMap&
      notifications = rewards_service_->GetAllNotifications();

  if (notifications.empty()) {
    FAIL() << "Should see Insufficient Funds notification";
    return;
  }

  bool is_showing_notification = IsShowingNotificationForType(
      RewardsNotificationType::REWARDS_NOTIFICATION_INSUFFICIENT_FUNDS);

  EXPECT_TRUE(is_showing_notification);
}

// Test whether rewards is disabled in private profile.
IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, PrefsTestInPrivateWindow) {
  rewards_browsertest_helper::EnableRewards(browser());
  EXPECT_TRUE(rewards_browsertest_util::IsRewardsEnabled(browser()));
  EXPECT_FALSE(rewards_browsertest_util::IsRewardsEnabled(browser(), true));
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, ProcessPendingContributions) {
  AddNotificationServiceObserver();

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

  // Check if verified notification is shown
  rewards_browsertest_util::WaitForElementToContain(
      popup_contents,
      "#root",
      "3zsistemi.si");

  // Close notification
  rewards_browsertest_util::WaitForElementThenClick(
      popup_contents,
      "[data-test-id=notification-close]");

  // Check if insufficient funds notification is shown
  rewards_browsertest_util::WaitForElementToContain(
      popup_contents,
      "#root",
      "Insufficient Funds");
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, PanelDefaultMonthlyTipChoices) {
  rewards_browsertest_helper::EnableRewards(browser());

  balance_ = promotion_->ClaimPromotionViaCode();

  GURL url = https_server()->GetURL("3zsistemi.si", "/index.html");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Add a recurring tip of 10 BAT.
  TipViaCode(
      "3zsistemi.si",
      10.0,
      ledger::PublisherStatus::VERIFIED,
      false,
      true);

  content::WebContents* popup =
      rewards_browsertest_helper::OpenRewardsPopup(browser());
  const auto tip_options = rewards_browsertest_util::GetRewardsPopupTipOptions(
      popup);
  ASSERT_EQ(tip_options, std::vector<double>({ 0, 1, 10, 100 }));
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, SiteBannerDefaultTipChoices) {
  rewards_browsertest_helper::EnableRewards(browser());

  GURL url = https_server()->GetURL("3zsistemi.si", "/index.html");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  content::WebContents* site_banner =
      OpenSiteBanner(rewards_browsertest_util::ContributionType::OneTimeTip);
  auto tip_options = rewards_browsertest_util::GetSiteBannerTipOptions(
      site_banner);
  ASSERT_EQ(tip_options, std::vector<double>({ 1, 5, 50 }));

  site_banner = OpenSiteBanner(
      rewards_browsertest_util::ContributionType::MonthlyTip);
  tip_options = rewards_browsertest_util::GetSiteBannerTipOptions(
      site_banner);
  ASSERT_EQ(tip_options, std::vector<double>({ 1, 10, 100 }));
}

IN_PROC_BROWSER_TEST_F(
    RewardsBrowserTest,
    SiteBannerDefaultPublisherAmounts) {
  rewards_browsertest_helper::EnableRewards(browser());

  GURL url = https_server()->GetURL("laurenwags.github.io", "/index.html");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  content::WebContents* site_banner =
      OpenSiteBanner(rewards_browsertest_util::ContributionType::OneTimeTip);
  const auto tip_options = rewards_browsertest_util::GetSiteBannerTipOptions(
      site_banner);
  ASSERT_EQ(tip_options, std::vector<double>({ 5, 10, 20 }));
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, NotVerifiedWallet) {
  rewards_browsertest_helper::EnableRewards(browser());

  // Click on verify button
  rewards_browsertest_util::WaitForElementThenClick(
      contents(),
      "#verify-wallet-button");

  // Click on verify button in on boarding
  rewards_browsertest_util::WaitForElementThenClick(
      contents(),
      "#on-boarding-verify-button");

  // Check if we are redirected to uphold
  {
    const GURL current_url = contents()->GetURL();
    ASSERT_TRUE(base::StartsWith(
        current_url.spec(),
        braveledger_uphold::GetUrl() + "/authorize/",
        base::CompareCase::INSENSITIVE_ASCII));
  }

  // Fake successful authentication
  ui_test_utils::NavigateToURLBlockUntilNavigationsComplete(
        browser(),
        uphold_auth_url(), 1);

  // Check if we are redirected to KYC page
  {
    const GURL current_url = contents()->GetURL();
    ASSERT_TRUE(base::StartsWith(
        current_url.spec(),
        braveledger_uphold::GetUrl() + "/signup/step2",
        base::CompareCase::INSENSITIVE_ASCII));
  }
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
  NewTabPageWidgetEnableRewards) {
  rewards_browsertest_helper::EnableRewards(browser(), true);
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, PanelDontDoRequests) {
  // Open the Rewards popup
  content::WebContents *popup_contents =
      rewards_browsertest_helper::OpenRewardsPopup(browser());
  ASSERT_TRUE(popup_contents);

  // Make sure that no request was made
  ASSERT_FALSE(response_->WasRequestMade());
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, ShowMonthlyIfACOff) {
  rewards_browsertest_util::EnableRewardsViaCode(browser(), rewards_service());
  rewards_service_->SetAutoContributeEnabled(false);

  GURL url = https_server()->GetURL("3zsistemi.si", "/");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Open the Rewards popup
  content::WebContents *popup_contents =
      rewards_browsertest_helper::OpenRewardsPopup(browser());
  ASSERT_TRUE(popup_contents);

  rewards_browsertest_util::WaitForElementToAppear(
      popup_contents,
      "#panel-donate-monthly");
}

IN_PROC_BROWSER_TEST_F(RewardsBrowserTest, ShowACPercentInThePanel) {
  rewards_browsertest_helper::EnableRewards(browser());

  VisitPublisher("3zsistemi.si", true);

  GURL url = https_server()->GetURL("3zsistemi.si", "/");
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP);

  // Open the Rewards popup
  content::WebContents *popup_contents =
      rewards_browsertest_helper::OpenRewardsPopup(browser());
  ASSERT_TRUE(popup_contents);

  const std::string score =
      rewards_browsertest_util::WaitForElementThenGetContent(
          popup_contents,
          "[data-test-id='attention-score']");
  EXPECT_NE(score.find("100%"), std::string::npos);
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

IN_PROC_BROWSER_TEST_F(
    RewardsBrowserTest,
    PromotionHasEmptyPublicKey) {
  response_->SetPromotionEmptyKey(true);
  rewards_browsertest_helper::EnableRewards(browser());

  promotion_->WaitForPromotionInitialization();
  rewards_browsertest_util::WaitForElementToAppear(
      rewards_browsertest_helper::OpenRewardsPopup(browser()),
      "[data-test-id=notification-close]",
      false);
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
