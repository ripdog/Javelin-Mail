#include "gui/search/SearchSessionPersistence.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace javelin::gui::search
{
    namespace
    {
        [[nodiscard]] std::optional<std::string> optionalStringSetting(const QSettings& settings,
                                                                       const QString& key)
        {
            const auto value = settings.value(key).toString();
            return value.isEmpty() ? std::nullopt : std::optional<std::string>{value.toStdString()};
        }

        [[nodiscard]] QJsonObject
        serializeEmailAddress(const javelin::jmap::domain::EmailAddress& address)
        {
            QJsonObject object;
            if (address.name.has_value())
            {
                object.insert(QStringLiteral("name"), QString::fromStdString(*address.name));
            }
            object.insert(QStringLiteral("email"), QString::fromStdString(address.email));
            return object;
        }

        [[nodiscard]] std::optional<javelin::jmap::domain::EmailAddress>
        deserializeEmailAddress(const QJsonValue& value)
        {
            if (!value.isObject())
            {
                return std::nullopt;
            }
            const auto object = value.toObject();
            const auto email = object.value(QStringLiteral("email")).toString();
            if (email.isEmpty())
            {
                return std::nullopt;
            }
            const auto name = object.value(QStringLiteral("name")).toString();
            return javelin::jmap::domain::EmailAddress{
                .name =
                    name.isEmpty() ? std::nullopt : std::optional<std::string>{name.toStdString()},
                .email = email.toStdString(),
            };
        }

        [[nodiscard]] QJsonObject
        serializeMessageListItem(const javelin::jmap::cache::MessageListItem& item)
        {
            QJsonObject object;
            object.insert(QStringLiteral("emailId"), QString::fromStdString(item.emailId));
            object.insert(QStringLiteral("threadId"), QString::fromStdString(item.threadId));
            if (item.subject.has_value())
            {
                object.insert(QStringLiteral("subject"), QString::fromStdString(*item.subject));
            }
            if (item.preview.has_value())
            {
                object.insert(QStringLiteral("preview"), QString::fromStdString(*item.preview));
            }
            object.insert(QStringLiteral("receivedAt"), QString::fromStdString(item.receivedAt));
            if (item.sentAt.has_value())
            {
                object.insert(QStringLiteral("sentAt"), QString::fromStdString(*item.sentAt));
            }
            object.insert(QStringLiteral("threadMessageCount"),
                          static_cast<qint64>(item.threadMessageCount));
            object.insert(QStringLiteral("hasAttachment"), item.hasAttachment);
            object.insert(QStringLiteral("isUnread"), item.isUnread);
            object.insert(QStringLiteral("isFlagged"), item.isFlagged);
            if (item.from.has_value())
            {
                object.insert(QStringLiteral("from"), serializeEmailAddress(*item.from));
            }
            QJsonArray mailboxNames;
            for (const auto& name : item.mailboxNames)
            {
                mailboxNames.push_back(QString::fromStdString(name));
            }
            object.insert(QStringLiteral("mailboxNames"), mailboxNames);
            return object;
        }

        [[nodiscard]] std::optional<javelin::jmap::cache::MessageListItem>
        deserializeMessageListItem(const QJsonValue& value)
        {
            if (!value.isObject())
            {
                return std::nullopt;
            }
            const auto object = value.toObject();
            const auto emailId = object.value(QStringLiteral("emailId")).toString();
            const auto threadId = object.value(QStringLiteral("threadId")).toString();
            const auto receivedAt = object.value(QStringLiteral("receivedAt")).toString();
            if (emailId.isEmpty() || threadId.isEmpty() || receivedAt.isEmpty())
            {
                return std::nullopt;
            }
            std::vector<std::string> mailboxNames;
            for (const auto& name : object.value(QStringLiteral("mailboxNames")).toArray())
            {
                mailboxNames.push_back(name.toString().toStdString());
            }
            return javelin::jmap::cache::MessageListItem{
                .emailId = emailId.toStdString(),
                .threadId = threadId.toStdString(),
                .subject = object.value(QStringLiteral("subject")).isUndefined()
                               ? std::nullopt
                               : std::optional<std::string>{object.value(QStringLiteral("subject"))
                                                                .toString()
                                                                .toStdString()},
                .preview = object.value(QStringLiteral("preview")).isUndefined()
                               ? std::nullopt
                               : std::optional<std::string>{object.value(QStringLiteral("preview"))
                                                                .toString()
                                                                .toStdString()},
                .receivedAt = receivedAt.toStdString(),
                .sentAt = object.value(QStringLiteral("sentAt")).isUndefined()
                              ? std::nullopt
                              : std::optional<std::string>{object.value(QStringLiteral("sentAt"))
                                                               .toString()
                                                               .toStdString()},
                .threadMessageCount = static_cast<std::uint64_t>(
                    object.value(QStringLiteral("threadMessageCount")).toInteger(1)),
                .hasAttachment = object.value(QStringLiteral("hasAttachment")).toBool(false),
                .isUnread = object.value(QStringLiteral("isUnread")).toBool(false),
                .isFlagged = object.value(QStringLiteral("isFlagged")).toBool(false),
                .from = deserializeEmailAddress(object.value(QStringLiteral("from"))),
                .mailboxNames = std::move(mailboxNames),
            };
        }

        void writeOptionalField(QSettings& settings, const QString& key,
                                const std::optional<std::string>& value)
        {
            if (value.has_value())
            {
                settings.setValue(key, QString::fromStdString(*value));
            }
            else
            {
                settings.remove(key);
            }
        }
    } // namespace

    PersistedSearchState readSearchSessionSettings(const QSettings& settings)
    {
        auto text = optionalStringSetting(settings, QStringLiteral("searchText"));
        if (!text.has_value())
        {
            text = optionalStringSetting(settings, QStringLiteral("query"));
        }

        const auto cachedItems =
            QJsonDocument::fromJson(settings.value(QStringLiteral("cachedItems")).toByteArray())
                .array();
        std::vector<javelin::jmap::cache::MessageListItem> items;
        items.reserve(static_cast<std::size_t>(cachedItems.size()));
        for (const auto& itemValue : cachedItems)
        {
            if (const auto item = deserializeMessageListItem(itemValue))
            {
                items.push_back(*item);
            }
        }

        return PersistedSearchState{
            .criteria =
                javelin::jmap::search::EmailSearchCriteria{
                    .text = std::move(text),
                    .with = optionalStringSetting(settings, QStringLiteral("searchWith")),
                    .from = optionalStringSetting(settings, QStringLiteral("searchFrom")),
                    .to = optionalStringSetting(settings, QStringLiteral("searchTo")),
                    .cc = optionalStringSetting(settings, QStringLiteral("searchCc")),
                    .bcc = optionalStringSetting(settings, QStringLiteral("searchBcc")),
                    .subject = optionalStringSetting(settings, QStringLiteral("searchSubject")),
                    .body = optionalStringSetting(settings, QStringLiteral("searchBody")),
                },
            .restored =
                javelin::app::RestoredSearchState{
                    .page =
                        javelin::app::MessageListPage{
                            .offset = static_cast<std::size_t>(
                                settings.value(QStringLiteral("offset"), 0).toULongLong()),
                            .position = static_cast<std::size_t>(
                                settings.value(QStringLiteral("position"), 0).toULongLong()),
                            .returnedLimit = static_cast<std::size_t>(
                                settings.value(QStringLiteral("returnedLimit"), 0).toULongLong()),
                            .total =
                                settings.value(QStringLiteral("total")).isValid()
                                    ? std::optional<std::size_t>{static_cast<std::size_t>(
                                          settings.value(QStringLiteral("total")).toULongLong())}
                                    : std::nullopt,
                            .queryState = settings.value(QStringLiteral("queryState"))
                                              .toString()
                                              .toStdString(),
                            .anchor = std::nullopt,
                            .items = std::move(items),
                            .cacheLoaded = true,
                            .refreshInFlight = false,
                            .stale = true,
                            .refreshError = {},
                        },
                    .mode = settings.value(QStringLiteral("onlineSearch"), false).toBool()
                                ? javelin::app::SearchMode::Online
                                : javelin::app::SearchMode::Local,
                    .sessionId =
                        settings.value(QStringLiteral("searchSessionId")).toString().toStdString(),
                },
        };
    }

    void writeSearchSessionSettings(QSettings& settings, const javelin::app::SearchSession& session)
    {
        settings.setValue(QStringLiteral("type"), QStringLiteral("search"));
        settings.setValue(QStringLiteral("query"), QString::fromStdString(session.query()));
        settings.setValue(QStringLiteral("onlineSearch"),
                          session.mode() == javelin::app::SearchMode::Online);
        settings.setValue(QStringLiteral("searchSessionId"),
                          QString::fromStdString(session.sessionId()));
        const auto& criteria = session.criteria();
        writeOptionalField(settings, QStringLiteral("searchText"), criteria.text);
        writeOptionalField(settings, QStringLiteral("searchWith"), criteria.with);
        writeOptionalField(settings, QStringLiteral("searchFrom"), criteria.from);
        writeOptionalField(settings, QStringLiteral("searchTo"), criteria.to);
        writeOptionalField(settings, QStringLiteral("searchCc"), criteria.cc);
        writeOptionalField(settings, QStringLiteral("searchBcc"), criteria.bcc);
        writeOptionalField(settings, QStringLiteral("searchSubject"), criteria.subject);
        writeOptionalField(settings, QStringLiteral("searchBody"), criteria.body);

        const auto& page = session.page();
        settings.setValue(QStringLiteral("offset"), static_cast<qulonglong>(page.offset));
        settings.setValue(QStringLiteral("position"), static_cast<qulonglong>(page.position));
        settings.setValue(QStringLiteral("returnedLimit"),
                          static_cast<qulonglong>(page.returnedLimit));
        settings.setValue(QStringLiteral("queryState"), QString::fromStdString(page.queryState));
        if (page.total.has_value())
        {
            settings.setValue(QStringLiteral("total"), static_cast<qulonglong>(*page.total));
        }
        else
        {
            settings.remove(QStringLiteral("total"));
        }
        QJsonArray items;
        for (const auto& item : page.items)
        {
            items.push_back(serializeMessageListItem(item));
        }
        settings.setValue(QStringLiteral("cachedItems"),
                          QJsonDocument{items}.toJson(QJsonDocument::Compact));
    }

} // namespace javelin::gui::search
