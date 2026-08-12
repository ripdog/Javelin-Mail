#include "app/AddressSuggestionStore.h"

#include "storage/sqlite/DatabaseConnection.h"

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include <QtConcurrentRun>

#include <utility>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] std::optional<QStringList> loadSuggestions(const QString& databasePath)
        {
            javelin::jmap::cache::ReadOnlyThreadConnectionFactory factory{
                {.connectionNamePrefix = QStringLiteral("address-suggestions"),
                 .databasePath = databasePath}};
            auto opened = factory.openForCurrentThread("snapshot");
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
            {
                qWarning().noquote() << "Open known email addresses" << error->message;
                return std::nullopt;
            }
            auto connection =
                std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(opened));
            QSqlQuery query{connection.database()};
            if (!query.exec(QStringLiteral(
                    "WITH known_addresses AS ("
                    "SELECT c.display_name,e.address,0 AS source FROM contact_emails e "
                    "JOIN contact_cards c ON c.account_id=e.account_id AND "
                    "c.contact_id=e.contact_id "
                    "UNION ALL SELECT display_name,address,1 FROM email_addresses "
                    "WHERE field_name='from') "
                    "SELECT display_name,address,MIN(source) FROM known_addresses "
                    "WHERE TRIM(address)<>'' GROUP BY LOWER(address) "
                    "ORDER BY MIN(source),COALESCE(display_name,address) COLLATE NOCASE,address "
                    "COLLATE NOCASE")))
            {
                qWarning().noquote() << "Load known email addresses" << query.lastError().text();
                return std::nullopt;
            }
            QStringList suggestions;
            while (query.next())
            {
                const QString name = query.value(0).toString().trimmed();
                const QString address = query.value(1).toString().trimmed();
                suggestions.push_back(name.isEmpty() ||
                                              name.compare(address, Qt::CaseInsensitive) == 0
                                          ? address
                                          : QStringLiteral("%1 <%2>").arg(name, address));
            }
            return suggestions;
        }
    } // namespace

    AddressSuggestionStore::AddressSuggestionStore()
    {
        m_refreshTimer.setSingleShot(true);
        m_refreshTimer.setInterval(750);
        connect(&m_refreshTimer, &QTimer::timeout, this, &AddressSuggestionStore::startRefresh);
        connect(&m_refreshWatcher, &QFutureWatcher<std::optional<QStringList>>::finished, this,
                [this]
                {
                    if (auto suggestions = m_refreshWatcher.result(); suggestions.has_value())
                        m_model.setStringList(std::move(*suggestions));
                    if (m_refreshPending)
                    {
                        m_refreshPending = false;
                        m_refreshTimer.start();
                    }
                });
    }

    AddressSuggestionStore& AddressSuggestionStore::instance()
    {
        static AddressSuggestionStore store;
        return store;
    }

    void AddressSuggestionStore::initialize(QString databasePath)
    {
        m_databasePath = std::move(databasePath);
        refresh();
    }

    QStringListModel& AddressSuggestionStore::model()
    {
        return m_model;
    }

    void AddressSuggestionStore::refresh()
    {
        if (m_databasePath.isEmpty())
            return;
        if (m_refreshWatcher.isRunning())
        {
            m_refreshPending = true;
            return;
        }
        m_refreshTimer.start();
    }

    void AddressSuggestionStore::startRefresh()
    {
        if (m_refreshWatcher.isRunning())
        {
            m_refreshPending = true;
            return;
        }
        m_refreshWatcher.setFuture(QtConcurrent::run(loadSuggestions, m_databasePath));
    }
} // namespace javelin::app
