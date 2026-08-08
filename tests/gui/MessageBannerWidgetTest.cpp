#include "gui/messageview/MessageBannerWidget.h"

#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("message banners keep one aligned row and close after their action buttons",
          "[gui][message-banner]")
{
    javelin::gui::messageview::MessageBannerWidget banner;
    banner.setText(QStringLiteral("A message banner label"));
    auto* firstButton = banner.addButton(QStringLiteral("First"));
    auto* secondButton = banner.addButton(QStringLiteral("Second"));

    auto* outerLayout = qobject_cast<QVBoxLayout*>(banner.layout());
    REQUIRE(outerLayout != nullptr);
    auto* primaryRow = banner.findChild<QWidget*>(QStringLiteral("messageBannerPrimaryRow"));
    REQUIRE(primaryRow != nullptr);
    auto* layout = qobject_cast<QHBoxLayout*>(primaryRow->layout());
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

    auto* previewLabel = banner.findChild<QLabel*>(QStringLiteral("messageBannerPreview"));
    REQUIRE(previewLabel != nullptr);
    CHECK(previewLabel->isHidden());
    banner.setButtonHoverText(firstButton, QStringLiteral("https://example.com/unsubscribe"));
    QEvent enterEvent{QEvent::Enter};
    QCoreApplication::sendEvent(firstButton, &enterEvent);
    CHECK_FALSE(previewLabel->isHidden());
    CHECK(previewLabel->text() == QStringLiteral("https://example.com/unsubscribe"));
    QEvent leaveEvent{QEvent::Leave};
    QCoreApplication::sendEvent(firstButton, &leaveEvent);
    CHECK(previewLabel->isHidden());

    bool dismissed = false;
    QObject::connect(&banner, &javelin::gui::messageview::MessageBannerWidget::dismissed,
                     [&dismissed] { dismissed = true; });
    closeButton->click();
    CHECK(dismissed);
}
