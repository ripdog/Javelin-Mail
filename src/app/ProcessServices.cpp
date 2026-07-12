#include "app/ProcessServices.h"

#include "app/InlineMessageSchemeHandler.h"
#include "app/LongPollCoordinator.h"

#include "jmap/JmapCore.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/MessageViewService.h"
#include "jmap/cache/QueryService.h"
#include "jmap/cache/SubmissionRepository.h"
#include "jmap/cache/TranslationCacheRepository.h"
#include "jmap/contacts/ContactIdentityLookup.h"
#include "jmap/contacts/ContactService.h"
#include "jmap/render/InlineMessageUrl.h"
#include "jmap/submission/ComposeService.h"

#include <QDir>
#include <QStandardPaths>
#include <QWebEngineProfile>

#include <memory>
#include <stdexcept>

namespace javelin::app
{

    namespace
    {

        [[nodiscard]] QString cacheDatabasePath()
        {
            const QString basePath =
                QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            QDir directory;
            directory.mkpath(basePath);
            return QDir(basePath).filePath(QStringLiteral("cache.sqlite3"));
        }

    } // namespace

    ProcessServices::ProcessServices()
    {
        auto databaseResult = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("javelin-gui-main"),
            .databasePath = cacheDatabasePath(),
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&databaseResult))
        {
            throw std::runtime_error(error->message.toStdString());
        }

        m_databaseConnection =
            std::get<javelin::jmap::cache::DatabaseConnection>(std::move(databaseResult));
        m_networkAccessManager = std::make_unique<QNetworkAccessManager>();
        m_longPollNetworkAccessManager = std::make_unique<QNetworkAccessManager>();
        m_transport =
            std::make_unique<javelin::jmap::api::QtNetworkTransport>(*m_networkAccessManager);
        m_methodTransport =
            std::make_unique<javelin::jmap::api::HttpJmapMethodTransport>(*m_transport);
        m_inlineMessageSchemeHandler =
            std::make_unique<InlineMessageSchemeHandler>(m_databaseConnection);
        QWebEngineProfile::defaultProfile()->installUrlSchemeHandler(
            javelin::jmap::render::inlineMessageUrlScheme().toUtf8(),
            m_inlineMessageSchemeHandler.get());
        m_jmapCore = std::make_unique<javelin::jmap::JmapCore>(m_databaseConnection, *m_transport,
                                                               *m_methodTransport);
        m_accountRepository =
            std::make_unique<javelin::jmap::cache::AccountRepository>(m_databaseConnection);
        m_contactRepository =
            std::make_unique<javelin::jmap::cache::ContactRepository>(m_databaseConnection);
        m_contactService = std::make_unique<javelin::jmap::contacts::ContactService>(
            m_databaseConnection, *m_transport, *m_methodTransport);
        m_contactIdentityLookup =
            std::make_unique<javelin::jmap::contacts::ContactIdentityLookup>(*m_contactRepository);
        m_identityRepository =
            std::make_unique<javelin::jmap::cache::IdentityRepository>(m_databaseConnection);
        m_messageViewService =
            std::make_unique<javelin::jmap::cache::MessageViewService>(m_databaseConnection);
        m_queryService = std::make_unique<javelin::jmap::cache::QueryService>(m_databaseConnection);
        m_translationCacheRepository =
            std::make_unique<javelin::jmap::cache::TranslationCacheRepository>(
                m_databaseConnection);
        m_submissionRepository =
            std::make_unique<javelin::jmap::cache::SubmissionRepository>(m_databaseConnection);
        m_composeService = std::make_unique<javelin::jmap::submission::ComposeService>(
            m_databaseConnection, *m_transport, *m_methodTransport, *m_jmapCore);
        m_longPollService = std::make_unique<LongPollCoordinator>(
            m_databaseConnection, *m_jmapCore, *m_methodTransport, *m_longPollNetworkAccessManager,
            *m_accountRepository, *m_queryService, *m_contactService);
    }

    ProcessServices::~ProcessServices() = default;

    javelin::jmap::JmapCore& ProcessServices::jmapCore()
    {
        return *m_jmapCore;
    }

    const javelin::jmap::JmapCore& ProcessServices::jmapCore() const
    {
        return *m_jmapCore;
    }

    javelin::jmap::cache::AccountRepository& ProcessServices::accountRepository()
    {
        return *m_accountRepository;
    }

    javelin::jmap::cache::ContactRepository& ProcessServices::contactRepository()
    {
        return *m_contactRepository;
    }

    javelin::jmap::contacts::ContactService& ProcessServices::contactService()
    {
        return *m_contactService;
    }

    javelin::jmap::contacts::ContactIdentityLookup& ProcessServices::contactIdentityLookup()
    {
        return *m_contactIdentityLookup;
    }

    javelin::jmap::cache::IdentityRepository& ProcessServices::identityRepository()
    {
        return *m_identityRepository;
    }

    javelin::jmap::cache::MessageViewService& ProcessServices::messageViewService()
    {
        return *m_messageViewService;
    }

    javelin::jmap::cache::QueryService& ProcessServices::queryService()
    {
        return *m_queryService;
    }

    javelin::jmap::cache::TranslationCacheRepository& ProcessServices::translationCacheRepository()
    {
        return *m_translationCacheRepository;
    }

    javelin::jmap::submission::ComposeService& ProcessServices::composeService()
    {
        return *m_composeService;
    }

    LongPollCoordinator& ProcessServices::longPollService()
    {
        return *m_longPollService;
    }

} // namespace javelin::app
