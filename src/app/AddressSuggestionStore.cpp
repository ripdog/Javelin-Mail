#include "app/AddressSuggestionStore.h"

#include "jmap/cache/Database.h"

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>

namespace javelin::app
{
    AddressSuggestionStore& AddressSuggestionStore::instance()
    {
        static AddressSuggestionStore store;
        return store;
    }

    void AddressSuggestionStore::initialize(javelin::jmap::cache::DatabaseConnection& connection)
    {
        m_connection = &connection;
        refresh();
    }

    QStringListModel& AddressSuggestionStore::model()
    {
        return m_model;
    }

    void AddressSuggestionStore::refresh()
    {
        if (m_connection == nullptr)
            return;
        QSqlQuery query{m_connection->database()};
        if (!query.exec(QStringLiteral(
                "WITH known_addresses AS ("
                "SELECT c.display_name,e.address,0 AS source FROM contact_emails e "
                "JOIN contact_cards c ON c.account_id=e.account_id AND c.contact_id=e.contact_id "
                "UNION ALL SELECT display_name,address,1 FROM email_addresses "
                "WHERE field_name='from') "
                "SELECT display_name,address,MIN(source) FROM known_addresses "
                "WHERE TRIM(address)<>'' GROUP BY LOWER(address) "
                "ORDER BY MIN(source),COALESCE(display_name,address) COLLATE NOCASE,address "
                "COLLATE NOCASE")))
        {
            qWarning().noquote() << "Load known email addresses" << query.lastError().text();
            return;
        }
        QStringList suggestions;
        while (query.next())
        {
            const QString name = query.value(0).toString().trimmed();
            const QString address = query.value(1).toString().trimmed();
            suggestions.push_back(name.isEmpty() || name.compare(address, Qt::CaseInsensitive) == 0
                                      ? address
                                      : QStringLiteral("%1 <%2>").arg(name, address));
        }
        m_model.setStringList(suggestions);
    }
} // namespace javelin::app
