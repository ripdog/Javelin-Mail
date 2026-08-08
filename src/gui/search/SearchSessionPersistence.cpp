#include "gui/search/SearchSessionPersistence.h"

#include "gui/messages/MessageListWindowPersistence.h"

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
                    .mode =
                        settings.value(settingKey(prefix, QStringLiteral("onlineSearch")), false)
                                .toBool()
                            ? javelin::app::SearchMode::Online
                            : javelin::app::SearchMode::Local,
                    .sessionId =
                        settings.value(settingKey(prefix, QStringLiteral("searchSessionId")))
                            .toString()
                            .toStdString(),
                    .windows =
                        javelin::gui::messages::readMessageListWindowManifest(settings, prefix),
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
        javelin::gui::messages::writeMessageListWindowManifest(settings, prefix,
                                                               state.restored.windows);
        const auto& criteria = state.criteria;
        writeOptionalField(settings, prefix, QStringLiteral("searchText"), criteria.text);
        writeOptionalField(settings, prefix, QStringLiteral("searchWith"), criteria.with);
        writeOptionalField(settings, prefix, QStringLiteral("searchFrom"), criteria.from);
        writeOptionalField(settings, prefix, QStringLiteral("searchTo"), criteria.to);
        writeOptionalField(settings, prefix, QStringLiteral("searchCc"), criteria.cc);
        writeOptionalField(settings, prefix, QStringLiteral("searchBcc"), criteria.bcc);
        writeOptionalField(settings, prefix, QStringLiteral("searchSubject"), criteria.subject);
        writeOptionalField(settings, prefix, QStringLiteral("searchBody"), criteria.body);
    }

} // namespace javelin::gui::search
