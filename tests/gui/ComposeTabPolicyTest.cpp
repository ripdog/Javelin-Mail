#include "gui/shell/ComposeTabPolicy.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using namespace javelin::gui::shell;

TEST_CASE("compose tab opening reuses an existing session")
{
    const std::array descriptors{
        ComposeTabDescriptor{.index = 2, .composeSessionId = "compose-one"},
        ComposeTabDescriptor{.index = 5, .composeSessionId = "compose-two"},
    };

    const auto plan = planComposeTabOpen(descriptors, {
                                                          .composeSessionId = "compose-two",
                                                          .subject = "Updated subject",
                                                      });

    REQUIRE(plan.existingIndex == 5);
    CHECK(plan.updateExistingTitle);
    CHECK(plan.title == "Updated subject");
}

TEST_CASE("compose tab reuse preserves its title without a subject")
{
    const std::array descriptors{
        ComposeTabDescriptor{.index = 1, .composeSessionId = "compose-one"},
    };

    const auto plan = planComposeTabOpen(descriptors, {
                                                          .composeSessionId = "compose-one",
                                                          .subject = std::nullopt,
                                                      });

    REQUIRE(plan.existingIndex == 1);
    CHECK_FALSE(plan.updateExistingTitle);
    CHECK(plan.title.empty());
}

TEST_CASE("new compose tabs use the subject or compose fallback title")
{
    const std::array<ComposeTabDescriptor, 0> descriptors{};

    const auto subjectPlan = planComposeTabOpen(descriptors, {
                                                                 .composeSessionId = "subject-tab",
                                                                 .subject = "Hello",
                                                             });
    CHECK_FALSE(subjectPlan.existingIndex.has_value());
    CHECK(subjectPlan.title == "Hello");

    const auto fallbackPlan = planComposeTabOpen(descriptors, {
                                                                  .composeSessionId = "empty-tab",
                                                                  .subject = std::nullopt,
                                                              });
    CHECK_FALSE(fallbackPlan.existingIndex.has_value());
    CHECK(fallbackPlan.title == "Compose");
}

TEST_CASE("compose close planning prioritizes in-flight operations")
{
    CHECK(planComposeTabClose({
              .operationInFlight = true,
              .closeWithoutPrompt = true,
              .emptyDraft = true,
              .savedDraft = true,
          }) == ComposeTabClosePlan::BlockWhileBusy);
}

TEST_CASE("compose close planning handles immediate and empty draft closure")
{
    CHECK(planComposeTabClose({.closeWithoutPrompt = true}) ==
          ComposeTabClosePlan::CloseImmediately);
    CHECK(planComposeTabClose({.emptyDraft = true}) ==
          ComposeTabClosePlan::DiscardWorkingCopyAndClose);
}

TEST_CASE("compose close planning distinguishes saved and unsaved drafts")
{
    CHECK(planComposeTabClose({.savedDraft = true}) == ComposeTabClosePlan::ConfirmKeepSavedDraft);
    CHECK(planComposeTabClose({.savedDraft = true, .hasUnsavedChanges = true}) ==
          ComposeTabClosePlan::ConfirmSaveOrDiscard);
    CHECK(
        planComposeTabClose({.emptyDraft = true, .savedDraft = true, .hasUnsavedChanges = true}) ==
        ComposeTabClosePlan::ConfirmSaveOrDiscard);
    CHECK(planComposeTabClose({}) == ComposeTabClosePlan::ConfirmSaveOrDiscard);
}
