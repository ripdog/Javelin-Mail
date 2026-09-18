#include "gui/shell/MailWorkspaceController.h"

#include "app/MailApplicationEventsPorts.h"
#include "app/MailboxSession.h"
#include "app/MessageListMaterializationPort.h"
#include "app/MessageListSessionFactory.h"

#include <QCoroTask>

#include <QObject>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace
{
    class FakeMailEvents final : public javelin::app::MailApplicationEventsPort
    {
      public:
        using MailApplicationEventsPort::MailApplicationEventsPort;

        [[nodiscard]] std::unordered_map<std::string, javelin::app::MailAccountStatus>
        accountStatuses() const override
        {
            return {};
        }
    };

    class IdleMaterializationPort final : public javelin::app::MessageListMaterializationPort
    {
      public:
        [[nodiscard]] javelin::app::MailboxObservationLease
        beginMailboxObservation(std::string, std::string) override
        {
            return {};
        }

        [[nodiscard]] QCoro::Task<javelin::app::MailboxWindowResult>
        requestMailboxWindow(javelin::app::MailboxWindowIntent) override
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("Unexpected mailbox materialization in workspace test."),
            };
        }

        [[nodiscard]] QCoro::Task<javelin::app::SearchWindowResult>
        requestSearchWindow(javelin::app::SearchWindowIntent) override
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("Unexpected search materialization in workspace test."),
            };
        }

        void ensureThread(javelin::app::ThreadMaterializationIntent) override
        {
        }

        void retireSearchWindow(std::string, std::string) override
        {
        }
    };

    class RecordingSessionFactory final : public javelin::app::MessageListSessionFactoryPort
    {
      public:
        RecordingSessionFactory(IdleMaterializationPort& materialization, FakeMailEvents& events)
            : m_materialization(materialization), m_events(events)
        {
        }

        [[nodiscard]] javelin::app::MailboxSession*
        createMailboxSession(std::string accountId, std::string mailboxId, QString title,
                             std::optional<std::string> role,
                             javelin::jmap::query::EmailListSort sort, const std::size_t windowSize,
                             std::optional<javelin::app::RestoredMailboxState> restored,
                             QObject* parent) override
        {
            ++mailboxCreateCount;
            return new javelin::app::MailboxSession(
                std::move(accountId), std::move(mailboxId), std::move(title), std::move(role), sort,
                QStringLiteral("/tmp/javelin-mail-workspace-controller-test.sqlite3"),
                m_materialization, windowSize, m_events, std::move(restored), parent);
        }

        [[nodiscard]] javelin::app::SearchSession*
        createSearchSession(std::string, javelin::jmap::search::EmailSearchCriteria,
                            javelin::jmap::query::EmailListSort, std::size_t,
                            std::optional<javelin::app::RestoredSearchState>, QObject*) override
        {
            return nullptr;
        }

        std::size_t mailboxCreateCount = 0;

      private:
        IdleMaterializationPort& m_materialization;
        FakeMailEvents& m_events;
    };
} // namespace

TEST_CASE("home mailbox activation reuses the live session for the same mailbox",
          "[gui][mail-workspace][cache-first]")
{
    IdleMaterializationPort materialization;
    FakeMailEvents events;
    RecordingSessionFactory factory{materialization, events};
    QObject sessionParent;
    javelin::gui::shell::MailWorkspaceController workspace{factory, 100, &sessionParent};

    CHECK(workspace.activateHomeMailbox("account-1", "inbox", QStringLiteral("Inbox"),
                                        std::optional<std::string>{"inbox"}) == 0);
    REQUIRE(workspace.tabs().size() == 1);
    auto* first =
        std::get<javelin::gui::shell::MailboxTabState>(workspace.tabs()[0].content).session;
    REQUIRE(first != nullptr);
    CHECK(factory.mailboxCreateCount == 1);

    CHECK(workspace.activateHomeMailbox("account-1", "inbox", QStringLiteral("Inbox renamed"),
                                        std::optional<std::string>{"inbox"}) == 0);
    auto* reused =
        std::get<javelin::gui::shell::MailboxTabState>(workspace.tabs()[0].content).session;
    CHECK(reused == first);
    CHECK(factory.mailboxCreateCount == 1);
    CHECK(reused->title() == QStringLiteral("Inbox renamed"));

    CHECK(workspace.activateHomeMailbox("account-1", "archive", QStringLiteral("Archive"),
                                        std::optional<std::string>{"archive"}) == 0);
    auto* replacement =
        std::get<javelin::gui::shell::MailboxTabState>(workspace.tabs()[0].content).session;
    REQUIRE(replacement != nullptr);
    CHECK(replacement != first);
    CHECK(factory.mailboxCreateCount == 2);
    CHECK(replacement->mailboxId() == "archive");
}

TEST_CASE("home mailbox activation resets an active quick filter",
          "[gui][mail-workspace][cache-first]")
{
    IdleMaterializationPort materialization;
    FakeMailEvents events;
    RecordingSessionFactory factory{materialization, events};
    QObject sessionParent;
    javelin::gui::shell::MailWorkspaceController workspace{factory, 100, &sessionParent};

    CHECK(workspace.activateHomeMailbox("account-1", "inbox", QStringLiteral("Inbox"),
                                        std::optional<std::string>{"inbox"}) == 0);
    auto* filtered =
        std::get<javelin::gui::shell::MailboxTabState>(workspace.tabs()[0].content).session;
    REQUIRE(filtered != nullptr);
    filtered->setQuickFilter({.unreadOnly = true});
    CHECK(filtered->quickFilterActive());

    CHECK(workspace.activateHomeMailbox("account-1", "inbox", QStringLiteral("Inbox"),
                                        std::optional<std::string>{"inbox"}) == 0);
    auto* replacement =
        std::get<javelin::gui::shell::MailboxTabState>(workspace.tabs()[0].content).session;
    REQUIRE(replacement != nullptr);
    CHECK(replacement != filtered);
    CHECK_FALSE(replacement->quickFilterActive());
    CHECK(factory.mailboxCreateCount == 2);
}
