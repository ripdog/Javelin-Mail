#pragma once

#include "jmap/cache/Database.h"

#include <memory>

class QNetworkAccessManager;

namespace javelin::jmap
{
    class JmapCore;
}

namespace javelin::jmap::contacts
{
    class ContactService;
    class ContactIdentityLookup;
} // namespace javelin::jmap::contacts

namespace javelin::app
{
    class InlineMessageSchemeHandler;
    class LongPollService;
} // namespace javelin::app

namespace javelin::jmap::api
{
    class QtNetworkTransport;
}

namespace javelin::jmap::cache
{
    class AccountRepository;
    class ContactRepository;
    class IdentityRepository;
    class MessageViewService;
    class QueryService;
    class SubmissionRepository;
    class TranslationCacheRepository;
} // namespace javelin::jmap::cache

namespace javelin::jmap::submission
{
    class ComposeService;
}

namespace javelin::app
{

    class ProcessServices
    {
      public:
        ProcessServices();
        ~ProcessServices();

        ProcessServices(const ProcessServices&) = delete;
        ProcessServices& operator=(const ProcessServices&) = delete;
        ProcessServices(ProcessServices&&) = delete;
        ProcessServices& operator=(ProcessServices&&) = delete;

        [[nodiscard]] javelin::jmap::JmapCore& jmapCore();
        [[nodiscard]] const javelin::jmap::JmapCore& jmapCore() const;
        [[nodiscard]] javelin::jmap::cache::AccountRepository& accountRepository();
        [[nodiscard]] javelin::jmap::cache::ContactRepository& contactRepository();
        [[nodiscard]] javelin::jmap::contacts::ContactService& contactService();
        [[nodiscard]] javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup();
        [[nodiscard]] javelin::jmap::cache::IdentityRepository& identityRepository();
        [[nodiscard]] javelin::jmap::cache::MessageViewService& messageViewService();
        [[nodiscard]] javelin::jmap::cache::QueryService& queryService();
        [[nodiscard]] javelin::jmap::cache::TranslationCacheRepository&
        translationCacheRepository();
        [[nodiscard]] javelin::jmap::submission::ComposeService& composeService();
        [[nodiscard]] javelin::app::LongPollService& longPollService();

      private:
        std::unique_ptr<javelin::jmap::JmapCore> m_jmapCore;
        javelin::jmap::cache::DatabaseConnection m_databaseConnection;
        std::unique_ptr<QNetworkAccessManager> m_networkAccessManager;
        std::unique_ptr<QNetworkAccessManager> m_longPollNetworkAccessManager;
        std::unique_ptr<javelin::jmap::api::QtNetworkTransport> m_transport;
        std::unique_ptr<InlineMessageSchemeHandler> m_inlineMessageSchemeHandler;
        std::unique_ptr<javelin::jmap::cache::AccountRepository> m_accountRepository;
        std::unique_ptr<javelin::jmap::cache::ContactRepository> m_contactRepository;
        std::unique_ptr<javelin::jmap::contacts::ContactService> m_contactService;
        std::unique_ptr<javelin::jmap::contacts::ContactIdentityLookup> m_contactIdentityLookup;
        std::unique_ptr<javelin::jmap::cache::IdentityRepository> m_identityRepository;
        std::unique_ptr<javelin::jmap::cache::MessageViewService> m_messageViewService;
        std::unique_ptr<javelin::jmap::cache::QueryService> m_queryService;
        std::unique_ptr<javelin::jmap::cache::TranslationCacheRepository>
            m_translationCacheRepository;
        std::unique_ptr<javelin::jmap::cache::SubmissionRepository> m_submissionRepository;
        std::unique_ptr<javelin::jmap::submission::ComposeService> m_composeService;
        std::unique_ptr<LongPollService> m_longPollService;
    };

} // namespace javelin::app
