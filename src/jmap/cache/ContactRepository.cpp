#include "jmap/cache/ContactRepository.h"
#include "jmap/api/Session.h"

#include <glaze/glaze.hpp>

#include <QSqlError>
#include <QSqlQuery>

#include <unordered_set>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return {.code = DatabaseErrorCode::QueryFailed,
                    .message = operation + QStringLiteral(": ") + query.lastError().text()};
        }

        [[nodiscard]] std::string rightsJson(const javelin::jmap::api::AddressBookRights& rights)
        {
            std::string json;
            static_cast<void>(glz::write_json(rights, json));
            return json;
        }

        [[nodiscard]] std::string shareWithJson(
            const std::optional<
                std::unordered_map<std::string, javelin::jmap::api::AddressBookRights>>& shareWith)
        {
            std::string json;
            static_cast<void>(glz::write_json(shareWith, json));
            return json;
        }

        [[nodiscard]] std::optional<DatabaseError>
        insertAddressBook(QSqlDatabase& database, const std::string_view accountId,
                          const javelin::jmap::api::AddressBook& item, const std::string_view state)
        {
            QSqlQuery book{database};
            book.prepare(
                QStringLiteral("INSERT INTO address_books (account_id, address_book_id, name, "
                               "description, sort_order, is_default, is_subscribed, "
                               "share_with_json, my_rights_json, state) VALUES (:account,:id,"
                               ":name,:description,:sort,:default,:subscribed,:share,:rights,"
                               ":state) ON CONFLICT(account_id,address_book_id) DO UPDATE SET "
                               "name=excluded.name,description=excluded.description,"
                               "sort_order=excluded.sort_order,is_default=excluded.is_default,"
                               "is_subscribed=excluded.is_subscribed,"
                               "share_with_json=excluded.share_with_json,"
                               "my_rights_json=excluded.my_rights_json,state=excluded.state"));
            book.bindValue(QStringLiteral(":account"),
                           QString::fromStdString(std::string{accountId}));
            book.bindValue(QStringLiteral(":id"), QString::fromStdString(item.id));
            book.bindValue(QStringLiteral(":name"), QString::fromStdString(item.name));
            book.bindValue(QStringLiteral(":description"),
                           item.description.has_value()
                               ? QVariant{QString::fromStdString(*item.description)}
                               : QVariant{});
            book.bindValue(QStringLiteral(":sort"), item.sortOrder);
            book.bindValue(QStringLiteral(":default"), item.isDefault ? 1 : 0);
            book.bindValue(QStringLiteral(":subscribed"), item.isSubscribed ? 1 : 0);
            book.bindValue(QStringLiteral(":share"),
                           QString::fromStdString(shareWithJson(item.shareWith)));
            book.bindValue(QStringLiteral(":rights"),
                           QString::fromStdString(rightsJson(item.myRights)));
            book.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
            if (!book.exec())
                return queryError(QStringLiteral("Upsert address book"), book);
            return std::nullopt;
        }

        [[nodiscard]] std::optional<DatabaseError>
        insertContact(QSqlDatabase& database,
                      const javelin::jmap::contacts::ContactSummary& contact)
        {
            QSqlQuery card{database};
            card.prepare(QStringLiteral(
                "INSERT INTO contact_cards (account_id, contact_id, uid, kind, display_name, "
                "organization, document_json) VALUES (:account, :id, :uid, :kind, :name, :org, "
                ":document) ON CONFLICT(account_id, contact_id) DO UPDATE SET uid=excluded.uid, "
                "kind=excluded.kind, display_name=excluded.display_name, "
                "organization=excluded.organization, document_json=excluded.document_json"));
            card.bindValue(QStringLiteral(":account"), QString::fromStdString(contact.accountId));
            card.bindValue(QStringLiteral(":id"), QString::fromStdString(contact.id));
            card.bindValue(QStringLiteral(":uid"), QString::fromStdString(contact.uid));
            card.bindValue(QStringLiteral(":kind"), QString::fromStdString(contact.kind));
            card.bindValue(QStringLiteral(":name"), QString::fromStdString(contact.displayName));
            card.bindValue(QStringLiteral(":org"),
                           contact.organization.has_value()
                               ? QVariant{QString::fromStdString(*contact.organization)}
                               : QVariant{});
            card.bindValue(QStringLiteral(":document"), QString::fromStdString(contact.document));
            if (!card.exec())
            {
                return queryError(QStringLiteral("Upsert contact card"), card);
            }

            QSqlQuery clearBooks{database};
            clearBooks.prepare(QStringLiteral("DELETE FROM contact_card_address_books WHERE "
                                              "account_id=:account AND contact_id=:id"));
            clearBooks.bindValue(QStringLiteral(":account"),
                                 QString::fromStdString(contact.accountId));
            clearBooks.bindValue(QStringLiteral(":id"), QString::fromStdString(contact.id));
            if (!clearBooks.exec())
            {
                return queryError(QStringLiteral("Clear contact address books"), clearBooks);
            }
            QSqlQuery clearEmails{database};
            clearEmails.prepare(
                QStringLiteral("DELETE FROM contact_emails WHERE account_id=:account "
                               "AND contact_id=:id"));
            clearEmails.bindValue(QStringLiteral(":account"),
                                  QString::fromStdString(contact.accountId));
            clearEmails.bindValue(QStringLiteral(":id"), QString::fromStdString(contact.id));
            if (!clearEmails.exec())
            {
                return queryError(QStringLiteral("Clear contact emails"), clearEmails);
            }

            QSqlQuery book{database};
            book.prepare(QStringLiteral("INSERT INTO contact_card_address_books "
                                        "(account_id, contact_id, address_book_id) VALUES "
                                        "(:account, :id, :book)"));
            for (const auto& bookId : contact.addressBookIds)
            {
                book.bindValue(QStringLiteral(":account"),
                               QString::fromStdString(contact.accountId));
                book.bindValue(QStringLiteral(":id"), QString::fromStdString(contact.id));
                book.bindValue(QStringLiteral(":book"), QString::fromStdString(bookId));
                if (!book.exec())
                {
                    return queryError(QStringLiteral("Insert contact address book"), book);
                }
            }

            QSqlQuery email{database};
            email.prepare(QStringLiteral(
                "INSERT INTO contact_emails (account_id, contact_id, "
                "entry_key, address, normalized_address, label, preference) "
                "VALUES (:account, :id, :key, :address, :normalized, :label, :pref)"));
            for (const auto& item : contact.emails)
            {
                email.bindValue(QStringLiteral(":account"),
                                QString::fromStdString(contact.accountId));
                email.bindValue(QStringLiteral(":id"), QString::fromStdString(contact.id));
                email.bindValue(QStringLiteral(":key"), QString::fromStdString(item.key));
                email.bindValue(QStringLiteral(":address"), QString::fromStdString(item.address));
                email.bindValue(
                    QStringLiteral(":normalized"),
                    QString::fromStdString(javelin::jmap::contacts::normalizeEmail(item.address)));
                email.bindValue(QStringLiteral(":label"),
                                item.label.has_value()
                                    ? QVariant{QString::fromStdString(*item.label)}
                                    : QVariant{});
                email.bindValue(QStringLiteral(":pref"),
                                item.preference.has_value()
                                    ? QVariant{static_cast<qulonglong>(*item.preference)}
                                    : QVariant{});
                if (!email.exec())
                {
                    return queryError(QStringLiteral("Insert contact email"), email);
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<javelin::jmap::contacts::ContactSummary>
        readContact(QSqlQuery& query)
        {
            javelin::jmap::api::ContactCard card{
                .id = query.value(1).toString().toStdString(),
                .uid = query.value(2).toString().toStdString(),
                .kind = query.value(3).toString().toStdString(),
                .document = query.value(6).toString().toStdString(),
            };
            return javelin::jmap::contacts::summarizeContact(
                query.value(0).toString().toStdString(), card);
        }

        [[nodiscard]] std::optional<DatabaseError>
        writeContacts(QSqlDatabase& database, const std::string_view accountId,
                      const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
                      const std::span<const std::string> destroyed)
        {
            if (!destroyed.empty())
            {
                QSqlQuery remove{database};
                remove.prepare(QStringLiteral(
                    "DELETE FROM contact_cards WHERE account_id=:account AND contact_id=:id"));
                for (const auto& id : destroyed)
                {
                    remove.bindValue(QStringLiteral(":account"),
                                     QString::fromStdString(std::string{accountId}));
                    remove.bindValue(QStringLiteral(":id"), QString::fromStdString(id));
                    if (!remove.exec())
                    {
                        return queryError(QStringLiteral("Delete contact"), remove);
                    }
                }
            }
            for (const auto& contact : contacts)
            {
                if (const auto error = insertContact(database, contact))
                {
                    return error;
                }
            }
            return std::nullopt;
        }
    } // namespace

    ContactRepository::ContactRepository(DatabaseConnection& connection) : m_connection(connection)
    {
    }

    std::optional<DatabaseError> ContactRepository::replaceAll(
        const std::string_view accountId, const std::vector<javelin::jmap::api::AddressBook>& books,
        const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
        const std::string_view addressBookState, const std::string_view contactState)
    {
        auto transactionResult =
            DatabaseTransaction::begin(m_connection, QStringLiteral("Replace contacts"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        if (const auto error =
                replaceAll(transaction, accountId, books, contacts, addressBookState, contactState))
            return error;
        if (const auto error = transaction.commit())
            return error;
        notifyChanged(accountId);
        return std::nullopt;
    }

    std::optional<DatabaseError> ContactRepository::replaceAll(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::vector<javelin::jmap::api::AddressBook>& books,
        const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
        const std::string_view addressBookState, const std::string_view contactState)
    {
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Contacts replacement requires a matching transaction"),
            };
        auto& database = m_connection.database();
        QSqlQuery clearCards{database};
        clearCards.prepare(QStringLiteral("DELETE FROM contact_cards WHERE account_id=:account"));
        clearCards.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(std::string{accountId}));
        if (!clearCards.exec())
            return queryError(QStringLiteral("Clear contact cards"), clearCards);
        QSqlQuery clear{database};
        clear.prepare(QStringLiteral("DELETE FROM address_books WHERE account_id=:account"));
        clear.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        if (!clear.exec())
            return queryError(QStringLiteral("Clear address books"), clear);
        for (const auto& item : books)
        {
            if (const auto error = insertAddressBook(database, accountId, item, addressBookState))
                return error;
        }
        for (const auto& contact : contacts)
        {
            if (const auto error = insertContact(database, contact))
                return error;
        }
        QSqlQuery state{database};
        state.prepare(
            QStringLiteral("INSERT INTO sync_state (account_id, object_type, query_key, "
                           "state_token) VALUES (:account,'ContactCard','',:state) "
                           "ON CONFLICT(account_id,object_type,query_key) DO UPDATE SET "
                           "state_token=excluded.state_token,updated_at=CURRENT_TIMESTAMP"));
        state.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        state.bindValue(QStringLiteral(":state"),
                        QString::fromStdString(std::string{contactState}));
        if (!state.exec())
            return queryError(QStringLiteral("Store contacts state"), state);
        return std::nullopt;
    }

    std::optional<DatabaseError> ContactRepository::replaceAddressBooks(
        const std::string_view accountId, const std::vector<javelin::jmap::api::AddressBook>& books,
        const std::string_view state)
    {
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Begin address book replacement"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
        {
            return *error;
        }
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        if (const auto error = replaceAddressBooks(transaction, accountId, books, state))
        {
            return error;
        }
        if (const auto error = transaction.commit())
        {
            return error;
        }
        notifyChanged(accountId);
        return std::nullopt;
    }

    std::optional<DatabaseError> ContactRepository::replaceAddressBooks(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::vector<javelin::jmap::api::AddressBook>& books, const std::string_view state)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message =
                    QStringLiteral("Address book replacement requires a matching transaction"),
            };
        }

        auto& database = m_connection.database();
        std::unordered_set<std::string> retainedIds;
        retainedIds.reserve(books.size());
        for (const auto& book : books)
        {
            retainedIds.insert(book.id);
            if (const auto error = insertAddressBook(database, accountId, book, state))
            {
                return error;
            }
        }

        QSqlQuery existing{database};
        existing.prepare(
            QStringLiteral("SELECT address_book_id FROM address_books WHERE account_id=:account"));
        existing.bindValue(QStringLiteral(":account"),
                           QString::fromStdString(std::string{accountId}));
        if (!existing.exec())
        {
            return queryError(QStringLiteral("List cached address books"), existing);
        }
        std::vector<std::string> removedIds;
        while (existing.next())
        {
            auto id = existing.value(0).toString().toStdString();
            if (!retainedIds.contains(id))
                removedIds.push_back(std::move(id));
        }

        QSqlQuery remove{database};
        remove.prepare(QStringLiteral("DELETE FROM address_books WHERE account_id=:account AND "
                                      "address_book_id=:id"));
        for (const auto& id : removedIds)
        {
            remove.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(std::string{accountId}));
            remove.bindValue(QStringLiteral(":id"), QString::fromStdString(id));
            if (!remove.exec())
            {
                return queryError(QStringLiteral("Remove stale address book"), remove);
            }
        }

        return std::nullopt;
    }

    std::optional<DatabaseError> ContactRepository::upsertContacts(
        const std::string_view accountId,
        const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
        const std::span<const std::string> destroyed, const std::string_view state)
    {
        auto transactionResult =
            DatabaseTransaction::begin(m_connection, QStringLiteral("Begin contact update"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
        {
            return *error;
        }
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        if (const auto error = upsertContacts(transaction, accountId, contacts, destroyed, state))
        {
            return error;
        }
        if (const auto error = transaction.commit())
        {
            return error;
        }
        notifyChanged(accountId);
        return std::nullopt;
    }

    std::optional<DatabaseError> ContactRepository::upsertContacts(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
        const std::span<const std::string> destroyed, const std::string_view state)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Contact update requires a matching transaction"),
            };
        }

        auto& database = m_connection.database();
        if (const auto error = writeContacts(database, accountId, contacts, destroyed))
        {
            return error;
        }
        QSqlQuery syncState{database};
        syncState.prepare(
            QStringLiteral("INSERT INTO sync_state (account_id,object_type,query_key,"
                           "state_token) VALUES (:account,'ContactCard','',:state) "
                           "ON CONFLICT(account_id,object_type,query_key) DO UPDATE SET "
                           "state_token=excluded.state_token,updated_at=CURRENT_TIMESTAMP"));
        syncState.bindValue(QStringLiteral(":account"),
                            QString::fromStdString(std::string{accountId}));
        syncState.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
        if (!syncState.exec())
        {
            return queryError(QStringLiteral("Store contact state"), syncState);
        }
        return std::nullopt;
    }

    std::optional<DatabaseError> ContactRepository::projectContacts(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
        const std::span<const std::string> destroyed)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Contact projection requires a matching transaction"),
            };
        }
        return writeContacts(m_connection.database(), accountId, contacts, destroyed);
    }

    void ContactRepository::notifyChanged(const std::string_view accountId)
    {
        Q_EMIT contactsChanged(QString::fromStdString(std::string{accountId}));
    }

    std::variant<std::vector<javelin::jmap::api::AddressBook>, DatabaseError>
    ContactRepository::listAddressBooks(const std::string_view accountId,
                                        const bool includeUnsubscribed) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("SELECT address_book_id,name,description,sort_order,is_default,"
                           "is_subscribed,share_with_json,my_rights_json FROM address_books WHERE "
                           "account_id=:account AND (:all OR is_subscribed=1) ORDER BY "
                           "sort_order,name COLLATE NOCASE"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":all"), includeUnsubscribed ? 1 : 0);
        if (!query.exec())
        {
            return queryError(QStringLiteral("List address books"), query);
        }
        std::vector<javelin::jmap::api::AddressBook> result;
        while (query.next())
        {
            javelin::jmap::api::AddressBookRights rights;
            auto shareJson = query.value(6).toString().toStdString();
            std::optional<std::unordered_map<std::string, javelin::jmap::api::AddressBookRights>>
                shareWith;
            static_cast<void>(
                glz::read<glz::opts{.error_on_unknown_keys = false}>(shareWith, shareJson));
            auto json = query.value(7).toString().toStdString();
            static_cast<void>(glz::read<glz::opts{.error_on_unknown_keys = false}>(rights, json));
            result.push_back(
                {.id = query.value(0).toString().toStdString(),
                 .name = query.value(1).toString().toStdString(),
                 .description = query.value(2).isNull()
                                    ? std::nullopt
                                    : std::optional{query.value(2).toString().toStdString()},
                 .sortOrder = query.value(3).toUInt(),
                 .isDefault = query.value(4).toInt() != 0,
                 .isSubscribed = query.value(5).toInt() != 0,
                 .shareWith = std::move(shareWith),
                 .myRights = rights});
        }
        return result;
    }

    std::variant<std::optional<std::string>, DatabaseError>
    ContactRepository::addressBookState(const std::string_view accountId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT state FROM address_books WHERE account_id=:account ORDER BY address_book_id "
            "LIMIT 1"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read AddressBook state"), query);
        if (!query.next() || query.value(0).isNull())
            return std::optional<std::string>{};
        return std::optional<std::string>{query.value(0).toString().toStdString()};
    }

    std::variant<std::vector<ContactAccount>, DatabaseError>
    ContactRepository::listAccounts(const std::optional<std::string_view> ownerAccountId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT account_id,owner_account_id,name,is_read_only,"
                                     "contacts_capabilities_json FROM accounts WHERE "
                                     "cap_contacts=1 AND (:owner='' OR owner_account_id=:owner) "
                                     "ORDER BY is_primary DESC,name COLLATE NOCASE,account_id"));
        query.bindValue(QStringLiteral(":owner"),
                        ownerAccountId.has_value()
                            ? QString::fromStdString(std::string{*ownerAccountId})
                            : QStringLiteral(""));
        if (!query.exec())
        {
            return queryError(QStringLiteral("List Contacts accounts"), query);
        }
        std::vector<ContactAccount> result;
        while (query.next())
        {
            javelin::jmap::api::ContactsCapability capability;
            auto json = query.value(4).toString().toStdString();
            static_cast<void>(
                glz::read<glz::opts{.error_on_unknown_keys = false}>(capability, json));
            result.push_back({.accountId = query.value(0).toString().toStdString(),
                              .ownerAccountId = query.value(1).toString().toStdString(),
                              .name = query.value(2).toString().toStdString(),
                              .isReadOnly = query.value(3).toInt() != 0,
                              .mayCreateAddressBook = capability.mayCreateAddressBook});
        }
        return result;
    }

    std::variant<std::vector<javelin::jmap::contacts::ContactSummary>, DatabaseError>
    ContactRepository::listContacts(const std::string_view accountId,
                                    const std::optional<std::string_view> addressBookId,
                                    const std::string_view filter) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("SELECT DISTINCT c.account_id,c.contact_id,c.uid,c.kind,"
                           "c.display_name,c.organization,c.document_json FROM contact_cards c "
                           "LEFT JOIN contact_card_address_books b ON b.account_id=c.account_id "
                           "AND b.contact_id=c.contact_id LEFT JOIN contact_emails e ON "
                           "e.account_id=c.account_id AND e.contact_id=c.contact_id WHERE "
                           "c.account_id=:account AND (:book='' OR b.address_book_id=:book) AND "
                           "(:filter='' OR c.display_name LIKE :pattern OR c.organization LIKE "
                           ":pattern OR e.address LIKE :pattern) ORDER BY c.display_name COLLATE "
                           "NOCASE,c.contact_id"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":book"),
                        addressBookId.has_value()
                            ? QString::fromStdString(std::string{*addressBookId})
                            : QStringLiteral(""));
        query.bindValue(QStringLiteral(":filter"), QString::fromStdString(std::string{filter}));
        const QString pattern =
            QStringLiteral("%") + QString::fromStdString(std::string{filter}) + QStringLiteral("%");
        query.bindValue(QStringLiteral(":pattern"), pattern);
        if (!query.exec())
        {
            return queryError(QStringLiteral("List contacts"), query);
        }
        std::vector<javelin::jmap::contacts::ContactSummary> result;
        while (query.next())
        {
            if (auto contact = readContact(query))
            {
                result.push_back(std::move(*contact));
            }
        }
        return result;
    }

    std::variant<std::optional<javelin::jmap::contacts::ContactSummary>, DatabaseError>
    ContactRepository::findContact(const std::string_view accountId,
                                   const std::string_view contactId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT account_id,contact_id,uid,kind,display_name,organization,document_json "
            "FROM contact_cards WHERE account_id=:account AND contact_id=:id"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{contactId}));
        if (!query.exec())
        {
            return queryError(QStringLiteral("Find contact"), query);
        }
        if (!query.next())
        {
            return std::optional<javelin::jmap::contacts::ContactSummary>{};
        }
        return readContact(query);
    }

    std::variant<std::optional<javelin::jmap::contacts::ContactSummary>, DatabaseError>
    ContactRepository::findByEmail(const std::string_view normalizedEmail,
                                   const std::optional<std::string_view> accountId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("SELECT c.account_id,c.contact_id,c.uid,c.kind,c.display_name,"
                           "c.organization,c.document_json FROM contact_cards c JOIN "
                           "contact_emails e ON e.account_id=c.account_id AND "
                           "e.contact_id=c.contact_id WHERE e.normalized_address=:email AND "
                           "(:account='' OR c.account_id=:account) ORDER BY e.preference IS NULL,"
                           "e.preference LIMIT 1"));
        query.bindValue(QStringLiteral(":email"),
                        QString::fromStdString(std::string{normalizedEmail}));
        query.bindValue(QStringLiteral(":account"),
                        accountId.has_value() ? QString::fromStdString(std::string{*accountId})
                                              : QStringLiteral(""));
        if (!query.exec())
        {
            return queryError(QStringLiteral("Find contact by email"), query);
        }
        if (!query.next())
        {
            return std::optional<javelin::jmap::contacts::ContactSummary>{std::nullopt};
        }
        return readContact(query);
    }
} // namespace javelin::jmap::cache
