#include "gui/messageview/MessageAttachmentPanel.h"

#include "gui/settings/GuiSettings.h"

#include <QApplication>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

TEST_CASE("attachment panel resizes without replacing interactive tiles", "[gui][messageview]")
{
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    const std::optional<std::string> accountId{"account-a"};
    const std::optional<std::string> emailId{"email-a"};

    javelin::jmap::cache::MessageViewSnapshot message;
    message.plainTextBody = javelin::jmap::cache::MessageBody{
        .kind = javelin::jmap::cache::MessageBodyKind::PlainText,
        .partId = "body",
        .isTruncated = false,
        .value = "Plain text body",
    };
    javelin::jmap::cache::MessageAttachment attachment;
    attachment.partId = "attachment-a";
    attachment.blobId = "blob-a";
    attachment.mediaType = "text/plain";
    attachment.name = "notes.txt";
    attachment.disposition = "attachment";
    attachment.size = 32;
    message.attachments.push_back(std::move(attachment));
    const std::optional<javelin::jmap::cache::MessageViewSnapshot> snapshot{std::move(message)};

    javelin::gui::messageview::MessageAttachmentPanel panel{settings, accountId, emailId, snapshot};
    panel.resize(900, 80);
    panel.show();
    QApplication::processEvents();

    auto* const originalTile = panel.findChild<QWidget*>(QStringLiteral("attachmentTile"));
    REQUIRE(originalTile != nullptr);
    auto* const originalOpenButton = [&]() -> QToolButton*
    {
        for (auto* button : originalTile->findChildren<QToolButton*>())
        {
            if (button->text() == QStringLiteral("notes.txt"))
                return button;
        }
        return nullptr;
    }();
    REQUIRE(originalOpenButton != nullptr);

    panel.resize(900, 81);
    QApplication::processEvents();
    CHECK(panel.findChild<QWidget*>(QStringLiteral("attachmentTile")) == originalTile);

    panel.resize(700, 81);
    QApplication::processEvents();
    CHECK(panel.findChild<QWidget*>(QStringLiteral("attachmentTile")) == originalTile);
    CHECK(originalOpenButton->isVisible());

    int openRequests = 0;
    QObject::connect(
        &panel, &javelin::gui::messageview::MessageAttachmentPanel::openAttachmentRequested,
        [&openRequests](const QString&, const QString&, const QString&) { ++openRequests; });
    originalOpenButton->click();
    CHECK(openRequests == 1);
}
