/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_promotion.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_context_util.h"
#include "brave/components/brave_rewards/browser/test/common/rewards_browsertest_context_helper.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace rewards_browsertest {

RewardsBrowserTestPromotion::RewardsBrowserTestPromotion() = default;

RewardsBrowserTestPromotion::~RewardsBrowserTestPromotion() = default;

void RewardsBrowserTestPromotion::Initialize(
      Browser* browser,
      brave_rewards::RewardsServiceImpl* rewards_service) {
  DCHECK(browser && rewards_service);
  browser_ = browser;
  rewards_service_ = rewards_service;

  rewards_service_->AddObserver(this);
}

void RewardsBrowserTestPromotion::WaitForPromotionInitialization() {
  if (initialized_) {
    return;
  }
  wait_for_initialization_loop_.reset(new base::RunLoop);
  wait_for_initialization_loop_->Run();
}

void RewardsBrowserTestPromotion::OnFetchPromotions(
    brave_rewards::RewardsService* rewards_service,
    unsigned int result,
    const std::vector<brave_rewards::Promotion>& promotions) {
  ASSERT_EQ(static_cast<ledger::Result>(result), ledger::Result::LEDGER_OK);
  initialized_ = true;

  if (wait_for_initialization_loop_) {
    wait_for_initialization_loop_->Quit();
  }
}

void RewardsBrowserTestPromotion::WaitForPromotionFinished() {
  if (finished_) {
    return;
  }

  wait_for_finished_loop_.reset(new base::RunLoop);
  wait_for_finished_loop_->Run();
}

void RewardsBrowserTestPromotion::OnPromotionFinished(
    brave_rewards::RewardsService* rewards_service,
    const uint32_t result,
    brave_rewards::Promotion promotion) {
  ASSERT_EQ(static_cast<ledger::Result>(result), ledger::Result::LEDGER_OK);
  finished_ = true;
  promotion_ = promotion;

  if (wait_for_finished_loop_) {
    wait_for_finished_loop_->Quit();
  }
}

brave_rewards::Promotion RewardsBrowserTestPromotion::GetPromotion() {
  return promotion_;
}

std::string RewardsBrowserTestPromotion::GetPromotionId() {
  return "6820f6a4-c6ef-481d-879c-d2c30c8928c3";
}

double RewardsBrowserTestPromotion::ClaimPromotion(bool use_panel) {
  // Wait for promotion to initialize
  WaitForPromotionInitialization();

  // Use the appropriate WebContents
  content::WebContents *contents = use_panel
      ? rewards_browsertest_helper::OpenRewardsPopup(browser_)
      : browser_->tab_strip_model()->GetActiveWebContents();

  // Claim promotion via settings page or panel, as instructed
  if (use_panel) {
    rewards_browsertest_util::WaitForElementThenClick(
        contents,
        "button");
  } else {
    rewards_browsertest_util::WaitForElementThenClick(
        contents,
        "[data-test-id='claimGrant']");
  }

  // Wait for CAPTCHA
  rewards_browsertest_util::WaitForElementToAppear(
      contents,
      "[data-test-id='captcha']");

  rewards_browsertest_util::DragAndDrop(
      contents,
      "[data-test-id=\"captcha-triangle\"]",
      "[data-test-id=\"captcha-drop\"]");

  WaitForPromotionFinished();

  // Ensure that promotion looks as expected
  auto promotion = GetPromotion();
  EXPECT_STREQ(std::to_string(promotion.amount).c_str(), "30.000000");
  EXPECT_STREQ(
      promotion.promotion_id.c_str(),
      GetPromotionId().c_str());
  EXPECT_EQ(promotion.type, 0u);
  EXPECT_EQ(promotion.expires_at, 1740816427ull);

  // Check that promotion notification shows the appropriate amount
  const std::string selector =
      use_panel ? "[id='root']" : "[data-test-id='newTokenGrant']";
  rewards_browsertest_util::WaitForElementToContain(
      contents,
      selector,
      "Free Token Grant");
  rewards_browsertest_util::WaitForElementToContain(
      contents,
      selector,
      "30.000 BAT");

  // Dismiss the promotion notification
  if (use_panel) {
    rewards_browsertest_util::WaitForElementThenClick(
        contents,
        "#"
        "grant-completed-ok");
  }

  return 30;
}

double RewardsBrowserTestPromotion::ClaimPromotionViaCode() {
  // Wait for promotion to initialize
  WaitForPromotionInitialization();

  const std::string solution = R"(
  {
    "captchaId": "a78e549f-904d-425e-9736-54f693117e01",
    "x": 1,
    "y": 1
  })";
  rewards_service_->AttestPromotion(
      GetPromotionId(),
      solution,
      base::DoNothing());
  WaitForPromotionFinished();
  return 30;
}

}  // namespace rewards_browsertest
