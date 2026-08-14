#include "gui/compose/UndoSendDialog.h"
#include "app/ComposeApplicationPorts.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

namespace
{
    class StubComposeCommands final : public javelin::app::ComposeCommandPort
    {
      public:
        QCoro::Task<
            std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
        open(javelin::app::AccountConnectionSettings,
             javelin::jmap::submission::OpenComposeRequest) override
        {
            co_return javelin::jmap::OperationError{};
        }

        QCoro::Task<std::variant<std::vector<javelin::jmap::domain::Identity>,
                                 javelin::jmap::OperationError>>
        loadSenderIdentities(javelin::app::AccountConnectionSettings, std::string) override
        {
            co_return std::vector<javelin::jmap::domain::Identity>{};
        }

        QCoro::Task<std::variant<javelin::jmap::submission::DraftSaveSummary,
                                 javelin::jmap::OperationError>>
        saveDraft(javelin::app::AccountConnectionSettings,
                  javelin::jmap::submission::DraftSnapshot) override
        {
            co_return javelin::jmap::OperationError{};
        }

        QCoro::Task<
            std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
        send(javelin::app::AccountConnectionSettings,
             javelin::jmap::submission::DraftSnapshot) override
        {
            co_return javelin::jmap::OperationError{};
        }

        QCoro::Task<
            std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
        scheduleSend(javelin::app::AccountConnectionSettings,
                     javelin::jmap::submission::ScheduledSendRequest) override
        {
            co_return javelin::jmap::OperationError{};
        }

        QCoro::Task<std::variant<bool, javelin::jmap::OperationError>>
        cancelDeferredSend(QString sendId) override
        {
            cancelledSendId = std::move(sendId);
            co_return true;
        }

        std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>,
                     javelin::jmap::OperationError>
        loadWorkingCopy(std::string_view) const override
        {
            return std::optional<javelin::jmap::submission::DraftSnapshot>{};
        }

        std::optional<javelin::jmap::OperationError>
        storeWorkingCopy(const javelin::jmap::submission::DraftSnapshot&) override
        {
            return std::nullopt;
        }

        std::optional<javelin::jmap::OperationError> discard(std::string_view) override
        {
            return std::nullopt;
        }

        QString cancelledSendId;
    };
} // namespace

TEST_CASE("undo send dialog is non-modal and exposes countdown controls", "[gui][compose]")
{
    StubComposeCommands commands;
    QWidget mainWindow;
    mainWindow.show();

    QPointer<javelin::gui::compose::UndoSendDialog> dialog =
        new javelin::gui::compose::UndoSendDialog(
            QStringLiteral("send-1"), QStringLiteral("Message scheduled"),
            QStringLiteral("Send “Quarterly report”"), QDateTime::currentMSecsSinceEpoch() + 5'000,
            commands, &mainWindow);
    dialog->show();
    QCoreApplication::processEvents();

    REQUIRE(dialog != nullptr);
    CHECK_FALSE(dialog->isModal());
    CHECK(dialog->windowModality() == Qt::NonModal);
    CHECK(mainWindow.isEnabled());
    CHECK(dialog->findChild<QProgressBar*>(QStringLiteral("undoSendProgress")) != nullptr);
    auto* undo = dialog->findChild<QPushButton*>(QStringLiteral("undoSendButton"));
    REQUIRE(undo != nullptr);
    CHECK(undo->isEnabled());

    undo->click();
    QCoreApplication::processEvents();
    CHECK(commands.cancelledSendId == QStringLiteral("send-1"));
    CHECK((dialog == nullptr || !dialog->isVisible()));
}
