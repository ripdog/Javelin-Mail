#include "gui/messageview/MessageBannerWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("message banners keep one aligned row and close after their action buttons",
          "[gui][message-banner]")
{
    javelin::gui::messageview::MessageBannerWidget banner;
    banner.setText(QStringLiteral("A message banner label"));
    auto* firstButton = banner.addButton(QStringLiteral("First"));
    auto* secondButton = banner.addButton(QStringLiteral("Second"));

    auto* layout = qobject_cast<QHBoxLayout*>(banner.layout());
    REQUIRE(layout != nullptr);
    REQUIRE(layout->count() == 5);
    CHECK(layout->itemAt(2)->widget() == firstButton);
    CHECK(layout->itemAt(3)->widget() == secondButton);

    auto* textLabel = banner.findChild<QLabel*>(QStringLiteral("messageBannerText"));
    REQUIRE(textLabel != nullptr);
    CHECK(textLabel->text() == QStringLiteral("A message banner label"));
    CHECK_FALSE(textLabel->wordWrap());
    CHECK(textLabel->sizePolicy().horizontalPolicy() == QSizePolicy::Expanding);
    CHECK(banner.sizePolicy().verticalPolicy() == QSizePolicy::Maximum);

    auto* closeButton = banner.findChild<QToolButton*>(QStringLiteral("messageBannerClose"));
    REQUIRE(closeButton != nullptr);
    CHECK(layout->itemAt(4)->widget() == closeButton);

    bool dismissed = false;
    QObject::connect(&banner, &javelin::gui::messageview::MessageBannerWidget::dismissed,
                     [&dismissed] { dismissed = true; });
    closeButton->click();
    CHECK(dismissed);
}
