#include "gui/messageview/InlineMessageSchemeHandler.h"

#include <QString>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("inline unavailable-image placeholders bound attachment labels",
          "[gui][messageview][inline]")
{
    const QString label(4096, QLatin1Char('&'));

    const QByteArray placeholder = javelin::app::unavailableInlineImagePlaceholder(label);

    CHECK(placeholder.size() < 4096);
    CHECK(placeholder.contains("&amp;"));
    CHECK(placeholder.contains(QByteArrayLiteral("\xE2\x80\xA6")));
    CHECK_FALSE(placeholder.contains(QByteArray(512, '&')));
}
