#include "gui/messageview/MessageViewPresentation.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("search multi-selection takes precedence over the missing mailbox placeholder",
          "[gui][message-view]")
{
    CHECK(javelin::gui::messageview::messageViewPresentation(true, false, false, 2) ==
          javelin::gui::messageview::MessageViewPresentation::MultipleSelection);
}

TEST_CASE("an empty search context still shows the missing mailbox placeholder",
          "[gui][message-view]")
{
    CHECK(javelin::gui::messageview::messageViewPresentation(true, false, false, 0) ==
          javelin::gui::messageview::MessageViewPresentation::NoMailbox);
}
