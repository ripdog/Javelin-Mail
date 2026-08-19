#include "gui/shell/EmailContextMenuLayout.h"

#include <catch2/catch_test_macros.hpp>

using namespace javelin::gui::shell;

TEST_CASE("email context menu defaults preserve safe action grouping")
{
    const auto& layout = defaultEmailContextMenuLayout();
    REQUIRE(layout.size() == 20);
    CHECK(layout.front() == QStringLiteral("compose_edit_draft"));
    CHECK(layout[4] == emailContextMenuSeparatorId());
    CHECK(layout[9] == emailContextMenuSeparatorId());
    CHECK(layout[13] == emailContextMenuSeparatorId());
    CHECK(layout[16] == emailContextMenuSeparatorId());
    CHECK(layout[17] == QStringLiteral("save_message"));
    CHECK(layout[18] == QStringLiteral("view_message_source"));
    CHECK(layout.back() == QStringLiteral("permanently_delete_email"));
}

TEST_CASE("email context menu normalization ignores invalid and duplicate actions")
{
    const auto normalized = normalizeEmailContextMenuLayout({
        emailContextMenuSeparatorId(),
        QStringLiteral("compose_reply"),
        emailContextMenuSeparatorId(),
        emailContextMenuSeparatorId(),
        QStringLiteral("unknown_action"),
        QStringLiteral("compose_reply"),
        QStringLiteral("archive_email"),
        emailContextMenuSeparatorId(),
    });

    CHECK(normalized == std::vector<QString>{QStringLiteral("compose_reply"),
                                             emailContextMenuSeparatorId(),
                                             QStringLiteral("archive_email")});
}

TEST_CASE("empty email context menu override selects the current defaults")
{
    CHECK(effectiveEmailContextMenuLayout({}) == defaultEmailContextMenuLayout());
    CHECK(effectiveEmailContextMenuLayout({QStringLiteral("archive_email")}) ==
          std::vector<QString>{QStringLiteral("archive_email")});
}

TEST_CASE("saving the default email context menu clears the override")
{
    CHECK(emailContextMenuOverrideForLayout(defaultEmailContextMenuLayout()).empty());
    CHECK(emailContextMenuOverrideForLayout({QStringLiteral("archive_email")}) ==
          std::vector<QString>{QStringLiteral("archive_email")});
}
