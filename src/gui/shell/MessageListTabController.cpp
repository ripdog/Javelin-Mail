#include "gui/shell/MessageListTabController.h"

#include "app/MailApplicationService.h"
#include "app/MessageListSession.h"
#include "jmap/cache/QueryService.h"
#include "jmap/search/EmailSearch.h"

#include <utility>

namespace javelin::gui::shell
{
    MessageListTabController::MessageListTabController(
        javelin::jmap::cache::QueryService& queryService,
        javelin::app::MailApplicationService& mailService, const std::size_t pageSize,
        QObject* sessionParent, QObject* parent)
        : QObject(parent), m_queryService(queryService), m_mailService(mailService),
          m_pageSize(pageSize), m_sessionParent(sessionParent)
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
        auto* session = new javelin::app::MailboxSession(
            std::move(spec.accountId), std::move(spec.mailboxId), std::move(spec.title),
            std::move(spec.role), spec.sort, m_queryService, m_mailService, m_pageSize,
            std::move(spec.restored), m_sessionParent);
        bind(*session);
        return {.content = MailboxTabState{.session = session, .selection = {}}};
    }

    TabState MessageListTabController::createSearchTab(SearchTabSessionSpec spec)
    {
        auto* session = new javelin::app::SearchSession(
            std::move(spec.accountId), std::move(spec.criteria), spec.sort, m_queryService,
            m_mailService, m_pageSize, std::move(spec.restored), m_sessionParent);
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

    bool MessageListTabController::loadCachedPage(TabState& tab, const bool forceReload)
    {
        auto* session = messageListSession(tab);
        if (session == nullptr)
            return false;

        session->loadCachedPage(forceReload);
        return true;
    }

    bool MessageListTabController::refresh(TabState& tab)
    {
        auto* session = messageListSession(tab);
        if (session == nullptr)
            return false;

        session->refresh();
        return true;
    }

    bool MessageListTabController::goToPreviousPage(TabState& tab)
    {
        auto* session = messageListSession(tab);
        return session != nullptr && session->goToPreviousPage();
    }

    bool MessageListTabController::goToNextPage(TabState& tab)
    {
        auto* session = messageListSession(tab);
        return session != nullptr && session->goToNextPage();
    }

    bool MessageListTabController::goToPage(TabState& tab, const std::size_t pageIndex)
    {
        auto* session = messageListSession(tab);
        return session != nullptr && session->goToPage(pageIndex);
    }

    std::optional<std::size_t> MessageListTabController::lastPageIndex(const TabState& tab) const
    {
        const auto* session = messageListSession(tab);
        if (session == nullptr || !session->page().total.has_value() || *session->page().total == 0)
        {
            return std::nullopt;
        }

        const auto effectiveLimit =
            session->page().returnedLimit == 0 ? m_pageSize : session->page().returnedLimit;
        return javelin::app::messageListPageCount(*session->page().total, effectiveLimit) - 1;
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

    bool MessageListTabController::refreshSearchAfterMutation(TabState& tab,
                                                              const std::string_view accountId)
    {
        auto* search = std::get_if<SearchTabState>(&tab.content);
        if (search == nullptr || search->session == nullptr ||
            search->session->accountId() != accountId)
        {
            return false;
        }

        search->session->refreshAfterMutation();
        return true;
    }

    bool MessageListTabController::pageStale(const TabState& tab) const
    {
        const auto* session = messageListSession(tab);
        return session != nullptr && session->page().stale;
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
        connect(&session, &javelin::app::MessageListSession::pageChanged, this,
                [this, session = &session] { Q_EMIT pageChanged(session); });
        connect(&session, &javelin::app::MessageListSession::refreshFailed, this,
                [this](const javelin::jmap::OperationError& error)
                { Q_EMIT operationFailed(error); });
    }
} // namespace javelin::gui::shell
