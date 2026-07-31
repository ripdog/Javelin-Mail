#include "app/GuiServices.h"

#include "app/AddressSuggestionStore.h"
#include "app/InlineMessageSchemeHandler.h"

#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/MessageViewService.h"
#include "jmap/cache/QueryService.h"
#include "jmap/calendar/CalendarReadService.h"
#include "jmap/calendar/CalendarReader.h"
#include "jmap/contacts/ContactIdentityLookup.h"
#include "jmap/render/InlineMessageUrl.h"

#include <QWebEngineProfile>

#include <stdexcept>
#include <utility>

namespace javelin::app
{
    GuiServices::GuiServices(QString databasePath, CacheAccessBarrier& cacheAccessBarrier,
                             javelin::jmap::cache::ContactRepository& contactWriter,
                             const bool installInlineMessageSchemeHandler)
        : m_databasePath(std::move(databasePath)), m_cacheAccessBarrier(cacheAccessBarrier)
    {
        auto opened = javelin::jmap::cache::GuiDatabaseFactory{
            javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
                .connectionNamePrefix = QStringLiteral("javelin-gui-read"),
                .databasePath = m_databasePath,
            }}.openForCurrentThread("main");
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
            throw std::runtime_error(error->message.toStdString());
        m_databaseConnection =
            std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(opened));

        m_cacheParticipant = m_cacheAccessBarrier.registerParticipant({
            .name = QStringLiteral("GUI cache connections"),
            .suspend = [this]() -> std::optional<javelin::jmap::cache::DatabaseError>
            {
                m_databaseConnection = javelin::jmap::cache::ReadOnlyDatabaseConnection{};
                return std::nullopt;
            },
            .resume = [this]() -> std::optional<javelin::jmap::cache::DatabaseError>
            {
                auto reopened = javelin::jmap::cache::GuiDatabaseFactory{
                    javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
                        .connectionNamePrefix = QStringLiteral("javelin-gui-read"),
                        .databasePath = m_databasePath,
                    }}.openForCurrentThread("main");
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&reopened))
                    return *error;
                m_databaseConnection =
                    std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(reopened));
                return std::nullopt;
            },
        });

        m_accountRepository =
            std::make_unique<javelin::jmap::cache::AccountReadRepository>(m_databaseConnection);
        m_mailboxRepository =
            std::make_unique<javelin::jmap::cache::MailboxReadRepository>(m_databaseConnection);
        m_contactRepository =
            std::make_unique<javelin::jmap::cache::ContactRepository>(m_databaseConnection);
        m_calendarService =
            std::make_unique<javelin::jmap::calendar::CalendarReadService>(m_databaseConnection);
        m_contactIdentityLookup =
            std::make_unique<javelin::jmap::contacts::ContactIdentityLookup>(*m_contactRepository);
        m_identityRepository =
            std::make_unique<javelin::jmap::cache::IdentityRepository>(m_databaseConnection);
        m_messageViewService =
            std::make_unique<javelin::jmap::cache::MessageViewService>(m_databaseConnection);
        m_queryService = std::make_unique<javelin::jmap::cache::QueryService>(m_databaseConnection);

        m_contactConnection = QObject::connect(
            &contactWriter, &javelin::jmap::cache::ContactRepository::contactsChanged,
            [this](const QString& accountId)
            {
                m_contactRepository->notifyChanged(accountId.toStdString());
                AddressSuggestionStore::instance().refresh();
            });
        AddressSuggestionStore::instance().initialize(m_databasePath);

        if (installInlineMessageSchemeHandler)
        {
            m_inlineMessageSchemeHandler =
                std::make_unique<InlineMessageSchemeHandler>(m_databaseConnection);
            QWebEngineProfile::defaultProfile()->installUrlSchemeHandler(
                javelin::jmap::render::inlineMessageUrlScheme().toUtf8(),
                m_inlineMessageSchemeHandler.get());
        }
    }

    GuiServices::~GuiServices()
    {
        QObject::disconnect(m_contactConnection);
        if (m_inlineMessageSchemeHandler != nullptr)
        {
            QWebEngineProfile::defaultProfile()->removeUrlSchemeHandler(
                m_inlineMessageSchemeHandler.get());
        }
        if (m_cacheParticipant != 0)
            m_cacheAccessBarrier.unregisterParticipant(m_cacheParticipant);
    }

    javelin::jmap::cache::AccountReader& GuiServices::accountReader()
    {
        return *m_accountRepository;
    }

    javelin::jmap::cache::MailboxReader& GuiServices::mailboxReader()
    {
        return *m_mailboxRepository;
    }

    javelin::jmap::cache::ContactReader& GuiServices::contactReader()
    {
        return *m_contactRepository;
    }

    javelin::jmap::calendar::CalendarReader& GuiServices::calendarReader()
    {
        return *m_calendarService;
    }

    javelin::jmap::contacts::ContactIdentityLookup& GuiServices::contactIdentityLookup()
    {
        return *m_contactIdentityLookup;
    }

    javelin::jmap::cache::IdentityReader& GuiServices::identityReader()
    {
        return *m_identityRepository;
    }

    javelin::jmap::cache::MessageViewReader& GuiServices::messageViewReader()
    {
        return *m_messageViewService;
    }

    javelin::jmap::cache::QueryReader& GuiServices::queryReader()
    {
        return *m_queryService;
    }

    AddressSuggestionStore& GuiServices::addressSuggestionStore()
    {
        return AddressSuggestionStore::instance();
    }
} // namespace javelin::app
