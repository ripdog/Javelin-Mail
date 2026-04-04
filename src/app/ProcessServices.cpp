#include "app/ProcessServices.h"

#include "jmap/JmapCore.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/QueryService.h"

#include <QDir>
#include <QStandardPaths>

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
            return QDir(basePath).filePath("cache.sqlite3");
        }

    } // namespace

    ProcessServices::ProcessServices() : m_jmapCore(std::make_unique<javelin::jmap::JmapCore>())
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
        m_accountRepository =
            std::make_unique<javelin::jmap::cache::AccountRepository>(m_databaseConnection);
        m_queryService = std::make_unique<javelin::jmap::cache::QueryService>(m_databaseConnection);
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

    javelin::jmap::cache::QueryService& ProcessServices::queryService()
    {
        return *m_queryService;
    }

} // namespace javelin::app
