#include "gui/search/SearchSessionPersistence.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace javelin::gui::search
{
    namespace
    {
        [[nodiscard]] QString settingKey(const QString& prefix, const QString& key)
        {
            return prefix + key;
        }

        [[nodiscard]] std::optional<std::string> optionalStringSetting(const QVariantMap& settings,
                                                                       const QString& prefix,
                                                                       const QString& key)
        {
            const auto value = settings.value(settingKey(prefix, key)).toString();
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

        void writeOptionalField(QVariantMap& settings, const QString& prefix, const QString& key,
                                const std::optional<std::string>& value)
        {
            const auto fullKey = settingKey(prefix, key);
            if (value.has_value())
                settings.insert(fullKey, QString::fromStdString(*value));
            else
                settings.remove(fullKey);
        }
    } // namespace

    PersistedSearchState readSearchSessionSettings(const QVariantMap& settings,
                                                   const QString& prefix)
    {
        const auto cachedItems =
            QJsonDocument::fromJson(
                settings.value(settingKey(prefix, QStringLiteral("cachedItems"))).toByteArray())
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
                    .text = optionalStringSetting(settings, prefix, QStringLiteral("searchText")),
                    .with = optionalStringSetting(settings, prefix, QStringLiteral("searchWith")),
                    .from = optionalStringSetting(settings, prefix, QStringLiteral("searchFrom")),
                    .to = optionalStringSetting(settings, prefix, QStringLiteral("searchTo")),
                    .cc = optionalStringSetting(settings, prefix, QStringLiteral("searchCc")),
                    .bcc = optionalStringSetting(settings, prefix, QStringLiteral("searchBcc")),
                    .subject =
                        optionalStringSetting(settings, prefix, QStringLiteral("searchSubject")),
                    .body = optionalStringSetting(settings, prefix, QStringLiteral("searchBody")),
                },
            .restored =
                javelin::app::RestoredSearchState{
                    .page =
                        javelin::app::MessageListPage{
                            .offset = static_cast<std::size_t>(
                                settings.value(settingKey(prefix, QStringLiteral("offset")), 0)
                                    .toULongLong()),
                            .installedOffset = std::nullopt,
                            .pendingOffset = std::nullopt,
                            .position = static_cast<std::size_t>(
                                settings.value(settingKey(prefix, QStringLiteral("position")), 0)
                                    .toULongLong()),
                            .returnedLimit = static_cast<std::size_t>(
                                settings
                                    .value(settingKey(prefix, QStringLiteral("returnedLimit")), 0)
                                    .toULongLong()),
                            .total =
                                settings.value(settingKey(prefix, QStringLiteral("total")))
                                        .isValid()
                                    ? std::optional<
                                          std::size_t>{static_cast<std::size_t>(settings
                                                                                    .value(
                                                                                        settingKey(
                                                                                            prefix, QStringLiteral(
                                                                                                        "total")))
                                                                                    .toULongLong())}
                                    : std::nullopt,
                            .queryState =
                                settings.value(settingKey(prefix, QStringLiteral("queryState")))
                                    .toString()
                                    .toStdString(),
                            .anchor = std::nullopt,
                            .items = std::move(items),
                            .cacheLoaded = true,
                            .refreshInFlight = false,
                            .stale = true,
                            .refreshError = {},
                        },
                    .mode =
                        settings.value(settingKey(prefix, QStringLiteral("onlineSearch")), false)
                                .toBool()
                            ? javelin::app::SearchMode::Online
                            : javelin::app::SearchMode::Local,
                    .sessionId =
                        settings.value(settingKey(prefix, QStringLiteral("searchSessionId")))
                            .toString()
                            .toStdString(),
                },
        };
    }

    void writeSearchSessionSettings(QVariantMap& settings, const QString& prefix,
                                    const PersistedSearchState& state)
    {
        settings.insert(settingKey(prefix, QStringLiteral("type")), QStringLiteral("search"));
        settings.insert(settingKey(prefix, QStringLiteral("onlineSearch")),
                        state.restored.mode == javelin::app::SearchMode::Online);
        settings.insert(settingKey(prefix, QStringLiteral("searchSessionId")),
                        QString::fromStdString(state.restored.sessionId));
        const auto& criteria = state.criteria;
        writeOptionalField(settings, prefix, QStringLiteral("searchText"), criteria.text);
        writeOptionalField(settings, prefix, QStringLiteral("searchWith"), criteria.with);
        writeOptionalField(settings, prefix, QStringLiteral("searchFrom"), criteria.from);
        writeOptionalField(settings, prefix, QStringLiteral("searchTo"), criteria.to);
        writeOptionalField(settings, prefix, QStringLiteral("searchCc"), criteria.cc);
        writeOptionalField(settings, prefix, QStringLiteral("searchBcc"), criteria.bcc);
        writeOptionalField(settings, prefix, QStringLiteral("searchSubject"), criteria.subject);
        writeOptionalField(settings, prefix, QStringLiteral("searchBody"), criteria.body);

        const auto& page = state.restored.page;
        settings.insert(settingKey(prefix, QStringLiteral("offset")),
                        static_cast<qulonglong>(page.offset));
        settings.insert(settingKey(prefix, QStringLiteral("position")),
                        static_cast<qulonglong>(page.position));
        settings.insert(settingKey(prefix, QStringLiteral("returnedLimit")),
                        static_cast<qulonglong>(page.returnedLimit));
        settings.insert(settingKey(prefix, QStringLiteral("queryState")),
                        QString::fromStdString(page.queryState));
        if (page.total.has_value())
        {
            settings.insert(settingKey(prefix, QStringLiteral("total")),
                            static_cast<qulonglong>(*page.total));
        }
        else
        {
            settings.remove(settingKey(prefix, QStringLiteral("total")));
        }
        QJsonArray items;
        for (const auto& item : page.items)
        {
            items.push_back(serializeMessageListItem(item));
        }
        settings.insert(settingKey(prefix, QStringLiteral("cachedItems")),
                        QJsonDocument{items}.toJson(QJsonDocument::Compact));
    }

} // namespace javelin::gui::search
