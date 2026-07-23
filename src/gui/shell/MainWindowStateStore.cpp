#include "gui/shell/MainWindowStateStore.h"

#include <QSettings>
#include <QStringList>

#include <algorithm>
#include <type_traits>

namespace javelin::gui::shell
{
    namespace
    {
        constexpr auto windowGroup = "mainWindow";
        constexpr auto geometryKey = "geometry";
        constexpr auto splitterKey = "splitterState";
        constexpr auto activeTabIndexKey = "activeTabIndex";
        constexpr auto emailListSortPropertyKey = "emailListSortProperty";
        constexpr auto emailListSortDirectionKey = "emailListSortDirection";
        constexpr auto tabsKey = "tabs";

        [[nodiscard]] std::optional<std::string> optionalString(const QSettings& settings,
                                                                const QString& key)
        {
            const auto value = settings.value(key).toString();
            return value.isEmpty() ? std::nullopt : std::optional<std::string>{value.toStdString()};
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

        [[nodiscard]] PersistedTabCommon readCommonTab(const QSettings& settings)
        {
            return {
                .accountId = settings.value(QStringLiteral("accountId")).toString().toStdString(),
                .title = settings.value(QStringLiteral("title")).toString(),
                .selection =
                    {
                        .threadId = optionalString(settings, QStringLiteral("threadId")),
                        .emailId = optionalString(settings, QStringLiteral("emailId")),
                    },
            };
        }

        void writeCommonTab(QSettings& settings, const PersistedTabCommon& common)
        {
            settings.setValue(QStringLiteral("accountId"),
                              QString::fromStdString(common.accountId));
            settings.setValue(QStringLiteral("title"), common.title);
            settings.setValue(QStringLiteral("threadId"),
                              common.selection.threadId
                                  ? QString::fromStdString(*common.selection.threadId)
                                  : QString{});
            settings.setValue(QStringLiteral("emailId"),
                              common.selection.emailId
                                  ? QString::fromStdString(*common.selection.emailId)
                                  : QString{});
        }

        [[nodiscard]] std::optional<PersistedTab> readTab(const QSettings& settings)
        {
            auto common = readCommonTab(settings);
            const auto type = settings.value(QStringLiteral("type")).toString();
            if (type.isEmpty() || common.accountId.empty())
                return std::nullopt;

            if (type == QStringLiteral("mailbox"))
            {
                const auto mailboxId =
                    settings.value(QStringLiteral("mailboxId")).toString().toStdString();
                if (mailboxId.empty())
                    return std::nullopt;
                return PersistedMailboxTab{
                    .common = std::move(common),
                    .mailboxId = mailboxId,
                    .mailboxRole = optionalString(settings, QStringLiteral("mailboxRole")),
                    .offset = static_cast<std::size_t>(
                        settings.value(QStringLiteral("offset"), 0).toULongLong()),
                };
            }
            if (type == QStringLiteral("search"))
            {
                return PersistedSearchTab{
                    .common = std::move(common),
                    .search = javelin::gui::search::readSearchSessionSettings(settings),
                };
            }
            if (type == QStringLiteral("compose"))
            {
                const auto composeSessionId =
                    settings.value(QStringLiteral("composeSessionId")).toString().toStdString();
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
                     settings.value(QStringLiteral("selectedContactKeys")).toStringList())
                    selectedContactKeys.push_back(key.toStdString());
                return PersistedContactsTab{
                    .common = std::move(common),
                    .view =
                        {
                            .accountId = settings.value(QStringLiteral("contactAccountId"))
                                             .toString()
                                             .toStdString(),
                            .addressBookId = settings.value(QStringLiteral("addressBookId"))
                                                 .toString()
                                                 .toStdString(),
                            .contactId = settings.value(QStringLiteral("contactId"))
                                             .toString()
                                             .toStdString(),
                            .filter = settings.value(QStringLiteral("contactFilter")).toString(),
                            .sortMode =
                                settings.value(QStringLiteral("contactSortMode"), 0).toInt(),
                            .groupFilterMode =
                                settings.value(QStringLiteral("contactGroupFilterMode"), 0).toInt(),
                            .groupId = settings.value(QStringLiteral("contactGroupId"))
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
                        settings.value(QStringLiteral("displayedMonth")).toString(), Qt::ISODate),
                };
            }
            return std::nullopt;
        }

        void writeTab(QSettings& settings, const PersistedTab& tab)
        {
            std::visit(
                [&settings](const auto& value)
                {
                    using Tab = std::decay_t<decltype(value)>;
                    writeCommonTab(settings, value.common);
                    if constexpr (std::is_same_v<Tab, PersistedMailboxTab>)
                    {
                        settings.setValue(QStringLiteral("type"), QStringLiteral("mailbox"));
                        settings.setValue(QStringLiteral("mailboxId"),
                                          QString::fromStdString(value.mailboxId));
                        settings.setValue(QStringLiteral("mailboxRole"),
                                          value.mailboxRole
                                              ? QString::fromStdString(*value.mailboxRole)
                                              : QString{});
                        settings.setValue(QStringLiteral("offset"),
                                          static_cast<qulonglong>(value.offset));
                    }
                    else if constexpr (std::is_same_v<Tab, PersistedSearchTab>)
                    {
                        javelin::gui::search::writeSearchSessionSettings(settings, value.search);
                    }
                    else if constexpr (std::is_same_v<Tab, PersistedComposeTab>)
                    {
                        settings.setValue(QStringLiteral("type"), QStringLiteral("compose"));
                        settings.setValue(QStringLiteral("composeSessionId"),
                                          QString::fromStdString(value.composeSessionId));
                    }
                    else if constexpr (std::is_same_v<Tab, PersistedContactsTab>)
                    {
                        settings.setValue(QStringLiteral("type"), QStringLiteral("contacts"));
                        settings.setValue(QStringLiteral("contactAccountId"),
                                          QString::fromStdString(value.view.accountId));
                        settings.setValue(QStringLiteral("addressBookId"),
                                          QString::fromStdString(value.view.addressBookId));
                        settings.setValue(QStringLiteral("contactId"),
                                          QString::fromStdString(value.view.contactId));
                        settings.setValue(QStringLiteral("contactFilter"), value.view.filter);
                        settings.setValue(QStringLiteral("contactSortMode"), value.view.sortMode);
                        settings.setValue(QStringLiteral("contactGroupFilterMode"),
                                          value.view.groupFilterMode);
                        settings.setValue(QStringLiteral("contactGroupId"),
                                          QString::fromStdString(value.view.groupId));
                        QStringList selectedContactKeys;
                        for (const auto& key : value.view.selectedContactKeys)
                            selectedContactKeys.push_back(QString::fromStdString(key));
                        settings.setValue(QStringLiteral("selectedContactKeys"),
                                          selectedContactKeys);
                    }
                    else if constexpr (std::is_same_v<Tab, PersistedCalendarTab>)
                    {
                        settings.setValue(QStringLiteral("type"), QStringLiteral("calendar"));
                        settings.setValue(QStringLiteral("displayedMonth"),
                                          value.displayedMonth.isValid()
                                              ? value.displayedMonth.toString(Qt::ISODate)
                                              : QString{});
                    }
                },
                tab);
        }
    } // namespace

    PersistedMainWindowState
    readMainWindowState(QSettings& settings, const javelin::jmap::query::EmailListSort defaultSort)
    {
        settings.beginGroup(QLatin1StringView{windowGroup});
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
        const auto tabCount = settings.beginReadArray(QLatin1StringView{tabsKey});
        state.tabs.reserve(static_cast<std::size_t>(std::max(0, tabCount)));
        for (int index = 0; index < tabCount; ++index)
        {
            settings.setArrayIndex(index);
            if (auto tab = readTab(settings))
                state.tabs.push_back(std::move(*tab));
        }
        settings.endArray();
        settings.endGroup();
        return state;
    }

    void writeMainWindowState(QSettings& settings, const PersistedMainWindowState& state)
    {
        settings.beginGroup(QLatin1StringView{windowGroup});
        settings.setValue(QLatin1StringView{geometryKey}, state.geometry);
        settings.setValue(QLatin1StringView{splitterKey}, state.splitterState);
        settings.setValue(QLatin1StringView{activeTabIndexKey}, state.activeTabIndex);
        settings.setValue(QLatin1StringView{emailListSortPropertyKey},
                          sortPropertyValue(state.emailListSort.property));
        settings.setValue(QLatin1StringView{emailListSortDirectionKey},
                          sortDirectionValue(state.emailListSort.direction));
        settings.beginWriteArray(QLatin1StringView{tabsKey});
        for (int index = 0; index < static_cast<int>(state.tabs.size()); ++index)
        {
            settings.setArrayIndex(index);
            writeTab(settings, state.tabs[static_cast<std::size_t>(index)]);
        }
        settings.endArray();
        settings.endGroup();
    }

    PersistedMainWindowState
    loadMainWindowState(const javelin::jmap::query::EmailListSort defaultSort)
    {
        QSettings settings;
        return readMainWindowState(settings, defaultSort);
    }

    void saveMainWindowState(const PersistedMainWindowState& state)
    {
        QSettings settings;
        writeMainWindowState(settings, state);
        settings.sync();
    }

    void saveEmailListSort(const javelin::jmap::query::EmailListSort sort)
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{windowGroup});
        settings.setValue(QLatin1StringView{emailListSortPropertyKey},
                          sortPropertyValue(sort.property));
        settings.setValue(QLatin1StringView{emailListSortDirectionKey},
                          sortDirectionValue(sort.direction));
        settings.endGroup();
        settings.sync();
    }
} // namespace javelin::gui::shell
