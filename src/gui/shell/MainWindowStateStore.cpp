#include "gui/shell/MainWindowStateStore.h"

#include <QDataStream>
#include <QIODevice>
#include <QStringList>
#include <QVariantMap>

#include <algorithm>
#include <type_traits>

namespace javelin::gui::shell
{
    namespace
    {
        constexpr auto geometryKey = "geometry";
        constexpr auto splitterKey = "splitterState";
        constexpr auto activeTabIndexKey = "activeTabIndex";
        constexpr auto emailListSortPropertyKey = "emailListSortProperty";
        constexpr auto emailListSortDirectionKey = "emailListSortDirection";
        constexpr auto tabsKey = "tabs";
        constexpr auto arraySizeKey = "size";
        constexpr int maximumPersistedTabs = 256;

        [[nodiscard]] QString settingKey(const QString& prefix, const QString& key)
        {
            return prefix + key;
        }

        [[nodiscard]] QString tabPrefix(const int index)
        {
            return QStringLiteral("%1/%2/").arg(QLatin1StringView{tabsKey}).arg(index + 1);
        }

        [[nodiscard]] std::optional<std::string>
        optionalString(const QVariantMap& settings, const QString& prefix, const QString& key)
        {
            const auto value = settings.value(settingKey(prefix, key)).toString();
            return value.isEmpty() ? std::nullopt : std::optional<std::string>{value.toStdString()};
        }

        void writeOptionalString(QVariantMap& settings, const QString& prefix, const QString& key,
                                 const std::optional<std::string>& value)
        {
            const auto fullKey = settingKey(prefix, key);
            if (value.has_value())
                settings.insert(fullKey, QString::fromStdString(*value));
            else
                settings.remove(fullKey);
        }

        [[nodiscard]] javelin::jmap::query::EmailListSortProperty sortProperty(const QString& value)
        {
            using enum javelin::jmap::query::EmailListSortProperty;
            if (value == QStringLiteral("sentAt"))
                return SentAt;
            if (value == QStringLiteral("from"))
                return From;
            if (value == QStringLiteral("to"))
                return To;
            if (value == QStringLiteral("subject"))
                return Subject;
            if (value == QStringLiteral("size"))
                return Size;
            return ReceivedAt;
        }

        [[nodiscard]] QString
        sortPropertyValue(const javelin::jmap::query::EmailListSortProperty property)
        {
            return QString::fromStdString(javelin::jmap::query::propertyName(property));
        }

        [[nodiscard]] javelin::jmap::query::EmailListSortDirection
        sortDirection(const QString& value)
        {
            return value == QStringLiteral("ascending")
                       ? javelin::jmap::query::EmailListSortDirection::Ascending
                       : javelin::jmap::query::EmailListSortDirection::Descending;
        }

        [[nodiscard]] QString
        sortDirectionValue(const javelin::jmap::query::EmailListSortDirection direction)
        {
            return direction == javelin::jmap::query::EmailListSortDirection::Ascending
                       ? QStringLiteral("ascending")
                       : QStringLiteral("descending");
        }

        [[nodiscard]] PersistedTabCommon readCommonTab(const QVariantMap& settings,
                                                       const QString& prefix)
        {
            return {
                .accountId = settings.value(settingKey(prefix, QStringLiteral("accountId")))
                                 .toString()
                                 .toStdString(),
                .title = settings.value(settingKey(prefix, QStringLiteral("title"))).toString(),
                .selection =
                    {
                        .threadId = optionalString(settings, prefix, QStringLiteral("threadId")),
                        .emailId = optionalString(settings, prefix, QStringLiteral("emailId")),
                    },
            };
        }

        void writeCommonTab(QVariantMap& settings, const QString& prefix,
                            const PersistedTabCommon& common)
        {
            settings.insert(settingKey(prefix, QStringLiteral("accountId")),
                            QString::fromStdString(common.accountId));
            settings.insert(settingKey(prefix, QStringLiteral("title")), common.title);
            writeOptionalString(settings, prefix, QStringLiteral("threadId"),
                                common.selection.threadId);
            writeOptionalString(settings, prefix, QStringLiteral("emailId"),
                                common.selection.emailId);
        }

        [[nodiscard]] std::optional<PersistedTab> readTab(const QVariantMap& settings,
                                                          const QString& prefix)
        {
            auto common = readCommonTab(settings, prefix);
            const auto type = settings.value(settingKey(prefix, QStringLiteral("type"))).toString();
            if (type.isEmpty())
                return std::nullopt;

            if (type == QStringLiteral("mailbox"))
            {
                if (common.accountId.empty())
                    return std::nullopt;
                const auto mailboxId =
                    settings.value(settingKey(prefix, QStringLiteral("mailboxId")))
                        .toString()
                        .toStdString();
                if (mailboxId.empty())
                    return std::nullopt;
                return PersistedMailboxTab{
                    .common = std::move(common),
                    .mailboxId = mailboxId,
                    .mailboxRole = optionalString(settings, prefix, QStringLiteral("mailboxRole")),
                    .offset = static_cast<std::size_t>(
                        settings.value(settingKey(prefix, QStringLiteral("offset")), 0)
                            .toULongLong()),
                };
            }
            if (type == QStringLiteral("search"))
            {
                if (common.accountId.empty())
                    return std::nullopt;
                return PersistedSearchTab{
                    .common = std::move(common),
                    .search = javelin::gui::search::readSearchSessionSettings(settings, prefix),
                };
            }
            if (type == QStringLiteral("compose"))
            {
                if (common.accountId.empty())
                    return std::nullopt;
                const auto composeSessionId =
                    settings.value(settingKey(prefix, QStringLiteral("composeSessionId")))
                        .toString()
                        .toStdString();
                if (composeSessionId.empty())
                    return std::nullopt;
                return PersistedComposeTab{
                    .common = std::move(common),
                    .composeSessionId = composeSessionId,
                };
            }
            if (type == QStringLiteral("contacts"))
            {
                std::vector<std::string> selectedContactKeys;
                for (const auto& key :
                     settings.value(settingKey(prefix, QStringLiteral("selectedContactKeys")))
                         .toStringList())
                    selectedContactKeys.push_back(key.toStdString());
                return PersistedContactsTab{
                    .common = std::move(common),
                    .view =
                        {
                            .accountId =
                                settings
                                    .value(settingKey(prefix, QStringLiteral("contactAccountId")))
                                    .toString()
                                    .toStdString(),
                            .addressBookId =
                                settings.value(settingKey(prefix, QStringLiteral("addressBookId")))
                                    .toString()
                                    .toStdString(),
                            .contactId =
                                settings.value(settingKey(prefix, QStringLiteral("contactId")))
                                    .toString()
                                    .toStdString(),
                            .filter =
                                settings.value(settingKey(prefix, QStringLiteral("contactFilter")))
                                    .toString(),
                            .sortMode =
                                settings
                                    .value(settingKey(prefix, QStringLiteral("contactSortMode")), 0)
                                    .toInt(),
                            .groupFilterMode =
                                settings
                                    .value(settingKey(prefix,
                                                      QStringLiteral("contactGroupFilterMode")),
                                           0)
                                    .toInt(),
                            .groupId =
                                settings.value(settingKey(prefix, QStringLiteral("contactGroupId")))
                                    .toString()
                                    .toStdString(),
                            .selectedContactKeys = std::move(selectedContactKeys),
                        },
                };
            }
            if (type == QStringLiteral("calendar"))
            {
                return PersistedCalendarTab{
                    .common = std::move(common),
                    .displayedMonth = QDate::fromString(
                        settings.value(settingKey(prefix, QStringLiteral("displayedMonth")))
                            .toString(),
                        Qt::ISODate),
                };
            }
            return std::nullopt;
        }

        void writeTab(QVariantMap& settings, const QString& prefix, const PersistedTab& tab)
        {
            std::visit(
                [&settings, &prefix](const auto& value)
                {
                    using Tab = std::decay_t<decltype(value)>;
                    writeCommonTab(settings, prefix, value.common);
                    if constexpr (std::is_same_v<Tab, PersistedMailboxTab>)
                    {
                        settings.insert(settingKey(prefix, QStringLiteral("type")),
                                        QStringLiteral("mailbox"));
                        settings.insert(settingKey(prefix, QStringLiteral("mailboxId")),
                                        QString::fromStdString(value.mailboxId));
                        writeOptionalString(settings, prefix, QStringLiteral("mailboxRole"),
                                            value.mailboxRole);
                        settings.insert(settingKey(prefix, QStringLiteral("offset")),
                                        static_cast<qulonglong>(value.offset));
                    }
                    else if constexpr (std::is_same_v<Tab, PersistedSearchTab>)
                    {
                        javelin::gui::search::writeSearchSessionSettings(settings, prefix,
                                                                         value.search);
                    }
                    else if constexpr (std::is_same_v<Tab, PersistedComposeTab>)
                    {
                        settings.insert(settingKey(prefix, QStringLiteral("type")),
                                        QStringLiteral("compose"));
                        settings.insert(settingKey(prefix, QStringLiteral("composeSessionId")),
                                        QString::fromStdString(value.composeSessionId));
                    }
                    else if constexpr (std::is_same_v<Tab, PersistedContactsTab>)
                    {
                        settings.insert(settingKey(prefix, QStringLiteral("type")),
                                        QStringLiteral("contacts"));
                        settings.insert(settingKey(prefix, QStringLiteral("contactAccountId")),
                                        QString::fromStdString(value.view.accountId));
                        settings.insert(settingKey(prefix, QStringLiteral("addressBookId")),
                                        QString::fromStdString(value.view.addressBookId));
                        settings.insert(settingKey(prefix, QStringLiteral("contactId")),
                                        QString::fromStdString(value.view.contactId));
                        settings.insert(settingKey(prefix, QStringLiteral("contactFilter")),
                                        value.view.filter);
                        settings.insert(settingKey(prefix, QStringLiteral("contactSortMode")),
                                        value.view.sortMode);
                        settings.insert(
                            settingKey(prefix, QStringLiteral("contactGroupFilterMode")),
                            value.view.groupFilterMode);
                        settings.insert(settingKey(prefix, QStringLiteral("contactGroupId")),
                                        QString::fromStdString(value.view.groupId));
                        QStringList selectedContactKeys;
                        for (const auto& key : value.view.selectedContactKeys)
                            selectedContactKeys.push_back(QString::fromStdString(key));
                        settings.insert(settingKey(prefix, QStringLiteral("selectedContactKeys")),
                                        selectedContactKeys);
                    }
                    else if constexpr (std::is_same_v<Tab, PersistedCalendarTab>)
                    {
                        settings.insert(settingKey(prefix, QStringLiteral("type")),
                                        QStringLiteral("calendar"));
                        settings.insert(settingKey(prefix, QStringLiteral("displayedMonth")),
                                        value.displayedMonth.isValid()
                                            ? value.displayedMonth.toString(Qt::ISODate)
                                            : QString{});
                    }
                },
                tab);
        }

        [[nodiscard]] QVariantMap decodeSettings(const QByteArray& encoded)
        {
            if (encoded.isEmpty())
                return {};
            QDataStream stream{encoded};
            stream.setByteOrder(QDataStream::BigEndian);
            stream.setVersion(QDataStream::Qt_6_6);
            QVariantMap settings;
            stream >> settings;
            if (stream.status() != QDataStream::Ok || !stream.atEnd())
                return {};
            return settings;
        }
    } // namespace

    QByteArray serializeMainWindowState(const PersistedMainWindowState& state)
    {
        QVariantMap settings;
        settings.insert(QLatin1StringView{geometryKey}, state.geometry);
        settings.insert(QLatin1StringView{splitterKey}, state.splitterState);
        settings.insert(QLatin1StringView{activeTabIndexKey}, state.activeTabIndex);
        settings.insert(QLatin1StringView{emailListSortPropertyKey},
                        sortPropertyValue(state.emailListSort.property));
        settings.insert(QLatin1StringView{emailListSortDirectionKey},
                        sortDirectionValue(state.emailListSort.direction));
        settings.insert(QStringLiteral("%1/%2").arg(QLatin1StringView{tabsKey},
                                                    QLatin1StringView{arraySizeKey}),
                        static_cast<int>(state.tabs.size()));
        for (int index = 0; index < static_cast<int>(state.tabs.size()); ++index)
            writeTab(settings, tabPrefix(index), state.tabs[static_cast<std::size_t>(index)]);

        QByteArray encoded;
        QDataStream stream{&encoded, QIODeviceBase::WriteOnly};
        stream.setByteOrder(QDataStream::BigEndian);
        stream.setVersion(QDataStream::Qt_6_6);
        stream << settings;
        return stream.status() == QDataStream::Ok ? encoded : QByteArray{};
    }

    PersistedMainWindowState
    deserializeMainWindowState(const QByteArray& encoded,
                               const javelin::jmap::query::EmailListSort defaultSort)
    {
        const auto settings = decodeSettings(encoded);
        PersistedMainWindowState state{
            .geometry = settings.value(QLatin1StringView{geometryKey}).toByteArray(),
            .splitterState = settings.value(QLatin1StringView{splitterKey}).toByteArray(),
            .activeTabIndex = settings.value(QLatin1StringView{activeTabIndexKey}, 0).toInt(),
            .emailListSort =
                {
                    .property = sortProperty(settings
                                                 .value(QLatin1StringView{emailListSortPropertyKey},
                                                        sortPropertyValue(defaultSort.property))
                                                 .toString()),
                    .direction =
                        sortDirection(settings
                                          .value(QLatin1StringView{emailListSortDirectionKey},
                                                 sortDirectionValue(defaultSort.direction))
                                          .toString()),
                },
            .tabs = {},
        };
        const auto tabCount =
            settings
                .value(QStringLiteral("%1/%2").arg(QLatin1StringView{tabsKey},
                                                   QLatin1StringView{arraySizeKey}),
                       0)
                .toInt();
        if (tabCount < 0 || tabCount > maximumPersistedTabs)
            return state;
        state.tabs.reserve(static_cast<std::size_t>(tabCount));
        for (int index = 0; index < tabCount; ++index)
        {
            if (auto tab = readTab(settings, tabPrefix(index)))
                state.tabs.push_back(std::move(*tab));
        }
        return state;
    }
} // namespace javelin::gui::shell
