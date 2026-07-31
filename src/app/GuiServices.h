#pragma once

#include "app/CacheAccessBarrier.h"
#include "jmap/cache/Database.h"

#include <QMetaObject>

#include <memory>

namespace javelin::jmap::cache
{
    class AccountReader;
    class AccountReadRepository;
    class MailboxReader;
    class MailboxReadRepository;
    class ContactReader;
    class ContactRepository;
    class IdentityReader;
    class IdentityRepository;
    class MessageViewReader;
    class MessageViewService;
    class QueryReader;
    class QueryService;
} // namespace javelin::jmap::cache

namespace javelin::jmap::calendar
{
    class CalendarReader;
    class CalendarReadService;
} // namespace javelin::jmap::calendar

namespace javelin::jmap::contacts
{
    class ContactIdentityLookup;
} // namespace javelin::jmap::contacts

namespace javelin::app
{
    class AddressSuggestionStore;
    class InlineMessageSchemeHandler;

    class GuiServices final
    {
      public:
        GuiServices(QString databasePath, CacheAccessBarrier& cacheAccessBarrier,
                    javelin::jmap::cache::ContactRepository& contactWriter,
                    bool installInlineMessageSchemeHandler = true);
        ~GuiServices();

        GuiServices(const GuiServices&) = delete;
        GuiServices& operator=(const GuiServices&) = delete;
        GuiServices(GuiServices&&) = delete;
        GuiServices& operator=(GuiServices&&) = delete;

        [[nodiscard]] javelin::jmap::cache::AccountReader& accountReader();
        [[nodiscard]] javelin::jmap::cache::MailboxReader& mailboxReader();
        [[nodiscard]] javelin::jmap::cache::ContactReader& contactReader();
        [[nodiscard]] javelin::jmap::calendar::CalendarReader& calendarReader();
        [[nodiscard]] javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup();
        [[nodiscard]] javelin::jmap::cache::IdentityReader& identityReader();
        [[nodiscard]] javelin::jmap::cache::MessageViewReader& messageViewReader();
        [[nodiscard]] javelin::jmap::cache::QueryReader& queryReader();
        [[nodiscard]] AddressSuggestionStore& addressSuggestionStore();

      private:
        QString m_databasePath;
        javelin::jmap::cache::ReadOnlyDatabaseConnection m_databaseConnection;
        CacheAccessBarrier& m_cacheAccessBarrier;
        CacheAccessBarrier::ParticipantId m_cacheParticipant = 0;
        QMetaObject::Connection m_contactConnection;
        std::unique_ptr<javelin::jmap::cache::AccountReadRepository> m_accountRepository;
        std::unique_ptr<javelin::jmap::cache::MailboxReadRepository> m_mailboxRepository;
        std::unique_ptr<javelin::jmap::cache::ContactRepository> m_contactRepository;
        std::unique_ptr<javelin::jmap::calendar::CalendarReadService> m_calendarService;
        std::unique_ptr<javelin::jmap::contacts::ContactIdentityLookup> m_contactIdentityLookup;
        std::unique_ptr<javelin::jmap::cache::IdentityRepository> m_identityRepository;
        std::unique_ptr<javelin::jmap::cache::MessageViewService> m_messageViewService;
        std::unique_ptr<javelin::jmap::cache::QueryService> m_queryService;
        std::unique_ptr<InlineMessageSchemeHandler> m_inlineMessageSchemeHandler;
    };
} // namespace javelin::app
