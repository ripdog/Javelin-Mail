#include "gui/shell/MessageListTabController.h"

#include "app/MailboxSession.h"
#include "app/MessageListSession.h"
#include "app/SearchSession.h"
#include "jmap/search/EmailSearch.h"

#include <utility>

namespace javelin::gui::shell
{
    MessageListTabController::MessageListTabController(
        javelin::app::MessageListSessionFactoryPort& sessionFactory, const std::size_t windowSize,
        QObject* sessionParent, QObject* parent)
        : QObject(parent), m_sessionFactory(sessionFactory), m_windowSize(windowSize),
          m_sessionParent(sessionParent)
    {
    }

    MessageListTabOpenResult
    MessageListTabController::openOrCreateMailbox(std::vector<TabState>& tabs,
                                                  MailboxTabSessionSpec spec,
                                                  const std::size_t firstReusableIndex)
    {
        const MessageListTabIdentity requested{
            .collection = MessageListTabCollection::Mailbox,
            .accountId = spec.accountId,
            .collectionKey = spec.mailboxId,
        };
        const auto index =
            findReusableMessageListTab(identities(tabs), requested, firstReusableIndex);
        if (index.has_value())
        {
            auto* mailbox = std::get_if<MailboxTabState>(&tabs[*index].content);
            if (mailbox != nullptr && mailbox->session != nullptr)
                mailbox->session->updateMetadata(std::move(spec.title), std::move(spec.role));
            return {.index = *index, .created = false};
        }

        tabs.push_back(createMailboxTab(std::move(spec)));
        return {.index = tabs.size() - 1, .created = true};
    }

    MessageListTabOpenResult
    MessageListTabController::openOrCreateSearch(std::vector<TabState>& tabs,
                                                 SearchTabSessionSpec spec,
                                                 const std::size_t firstReusableIndex)
    {
        const MessageListTabIdentity requested{
            .collection = MessageListTabCollection::Search,
            .accountId = spec.accountId,
            .collectionKey = javelin::jmap::search::displayString(spec.criteria),
        };
        const auto index =
            findReusableMessageListTab(identities(tabs), requested, firstReusableIndex);
        if (index.has_value())
            return {.index = *index, .created = false};

        tabs.push_back(createSearchTab(std::move(spec)));
        return {.index = tabs.size() - 1, .created = true};
    }

    TabState MessageListTabController::createMailboxTab(MailboxTabSessionSpec spec)
    {
        auto* session = m_sessionFactory.createMailboxSession(
            std::move(spec.accountId), std::move(spec.mailboxId), std::move(spec.title),
            std::move(spec.role), spec.sort, m_windowSize, std::move(spec.restored),
            m_sessionParent);
        bind(*session);
        return {.content = MailboxTabState{.session = session, .selection = {}}};
    }

    TabState MessageListTabController::createSearchTab(SearchTabSessionSpec spec)
    {
        auto* session = m_sessionFactory.createSearchSession(
            std::move(spec.accountId), std::move(spec.criteria), spec.sort, m_windowSize,
            std::move(spec.restored), m_sessionParent);
        bind(*session);
        return {.content = SearchTabState{.session = session, .selection = {}}};
    }

    void MessageListTabController::markTabsStaleForAccount(
        std::vector<TabState>& tabs, const std::string_view accountId,
        const std::optional<std::string_view> refreshedMailboxId)
    {
        for (const auto index :
             messageListTabsToMarkStale(identities(tabs), accountId, refreshedMailboxId))
        {
            if (auto* session = messageListSession(tabs[index]); session != nullptr)
                session->markStale();
        }
    }

    void MessageListTabController::markSearchTabsStaleForAccount(std::vector<TabState>& tabs,
                                                                 const std::string_view accountId)
    {
        for (const auto index :
             messageListTabsToMarkStale(identities(tabs), accountId, std::nullopt, true))
        {
            if (auto* session = messageListSession(tabs[index]); session != nullptr)
                session->markStale();
        }
    }

    bool MessageListTabController::loadCachedState(TabState& tab, const bool forceReload)
    {
        auto* session = messageListSession(tab);
        if (session == nullptr)
            return false;
        session->loadCachedState(forceReload);
        return true;
    }

    bool MessageListTabController::refresh(TabState& tab,
                                           const javelin::app::MessageListRefreshMode mode)
    {
        auto* session = messageListSession(tab);
        if (session == nullptr)
            return false;
        session->refresh(mode);
        return true;
    }

    bool MessageListTabController::canLoadMore(const TabState& tab) const
    {
        const auto* session = messageListSession(tab);
        return session != nullptr && session->canLoadMore();
    }

    bool MessageListTabController::loadMore(TabState& tab)
    {
        auto* session = messageListSession(tab);
        return session != nullptr && session->loadMore();
    }

    void MessageListTabController::setSort(std::vector<TabState>& tabs,
                                           const javelin::jmap::query::EmailListSort sort)
    {
        for (auto& tab : tabs)
        {
            if (auto* session = messageListSession(tab); session != nullptr)
                session->setSort(sort);
        }
    }

    bool MessageListTabController::stateStale(const TabState& tab) const
    {
        const auto* session = messageListSession(tab);
        return session != nullptr && session->state().stale;
    }

    bool MessageListTabController::stateRefreshInFlight(const TabState& tab) const
    {
        const auto* session = messageListSession(tab);
        return session != nullptr && session->state().refreshInFlight;
    }

    bool
    MessageListTabController::ownsSession(const TabState& tab,
                                          const javelin::app::MessageListSession* session) const
    {
        return session != nullptr && messageListSession(tab) == session;
    }

    bool MessageListTabController::promoteSearch(TabState& tab)
    {
        auto* search = std::get_if<SearchTabState>(&tab.content);
        if (search == nullptr || search->session == nullptr)
            return false;
        search->session->promoteToOnline();
        return true;
    }

    bool MessageListTabController::reveal(TabState& tab, std::string emailId)
    {
        auto* mailbox = std::get_if<MailboxTabState>(&tab.content);
        if (mailbox == nullptr || mailbox->session == nullptr)
            return false;
        mailbox->session->reveal(std::move(emailId));
        return true;
    }

    void MessageListTabController::releaseSession(TabState& tab)
    {
        if (auto* mailbox = std::get_if<MailboxTabState>(&tab.content);
            mailbox != nullptr && mailbox->session != nullptr)
        {
            mailbox->session->deleteLater();
            mailbox->session = nullptr;
            return;
        }

        if (auto* search = std::get_if<SearchTabState>(&tab.content);
            search != nullptr && search->session != nullptr)
        {
            search->session->close();
            search->session->deleteLater();
            search->session = nullptr;
        }
    }

    std::vector<std::optional<MessageListTabIdentity>>
    MessageListTabController::identities(const std::vector<TabState>& tabs) const
    {
        std::vector<std::optional<MessageListTabIdentity>> result;
        result.reserve(tabs.size());
        for (const auto& tab : tabs)
        {
            if (const auto* mailbox = std::get_if<MailboxTabState>(&tab.content);
                mailbox != nullptr && mailbox->session != nullptr)
            {
                result.push_back(MessageListTabIdentity{
                    .collection = MessageListTabCollection::Mailbox,
                    .accountId = mailbox->session->accountId(),
                    .collectionKey = mailbox->session->mailboxId(),
                });
            }
            else if (const auto* search = std::get_if<SearchTabState>(&tab.content);
                     search != nullptr && search->session != nullptr)
            {
                result.push_back(MessageListTabIdentity{
                    .collection = MessageListTabCollection::Search,
                    .accountId = search->session->accountId(),
                    .collectionKey = search->session->query(),
                });
            }
            else
            {
                result.push_back(std::nullopt);
            }
        }
        return result;
    }

    void MessageListTabController::bind(javelin::app::MessageListSession& session)
    {
        connect(&session, &javelin::app::MessageListSession::stateChanged, this,
                [this, session = &session] { Q_EMIT stateChanged(session); });
        connect(&session, &javelin::app::MessageListSession::refreshFailed, this,
                [this](const javelin::jmap::OperationError& error)
                { Q_EMIT operationFailed(error); });
    }
} // namespace javelin::gui::shell
