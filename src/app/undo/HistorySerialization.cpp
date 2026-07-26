#include "app/undo/HistorySerialization.h"

#include <glaze/glaze.hpp>

#include <string>
#include <type_traits>

namespace javelin::app::undo
{

    namespace
    {
        template <typename Payload>
        [[nodiscard]] std::variant<SerializedHistoryPayload, HistorySerializationError>
        serialize(const Payload& payload)
        {
            std::string json;
            if (const auto error = glz::write_json(payload, json))
            {
                return HistorySerializationError{
                    .message = QStringLiteral("Serialize history payload: ") +
                               QString::fromStdString(glz::format_error(error, json)),
                };
            }
            return SerializedHistoryPayload{
                .version = 1,
                .json = QString::fromStdString(json),
            };
        }

        template <typename Payload>
        [[nodiscard]] std::variant<HistoryPayload, HistorySerializationError>
        deserialize(const QString& json)
        {
            Payload payload;
            const auto buffer = json.toStdString();
            if (const auto error =
                    glz::read<glz::opts{.error_on_unknown_keys = false}>(payload, buffer))
            {
                return HistorySerializationError{
                    .message = QStringLiteral("Deserialize history payload: ") +
                               QString::fromStdString(glz::format_error(error, buffer)),
                };
            }
            return HistoryPayload{std::move(payload)};
        }
    } // namespace

    std::variant<SerializedHistoryPayload, HistorySerializationError>
    serializeHistoryPayload(const HistoryPayload& payload)
    {
        return std::visit([](const auto& value) { return serialize(value); }, payload);
    }

    std::variant<HistoryPayload, HistorySerializationError>
    deserializeHistoryPayload(const QString& commandKind, const int version, const QString& json)
    {
        if (version != 1)
        {
            return HistorySerializationError{
                .message = QStringLiteral("Unsupported history payload version %1 for %2")
                               .arg(version)
                               .arg(commandKind),
            };
        }

        if (commandKind == QStringLiteral("mail_patch"))
            return deserialize<MailPatchHistory>(json);
        if (commandKind == QStringLiteral("draft"))
            return deserialize<DraftHistory>(json);
        if (commandKind == QStringLiteral("sieve"))
            return deserialize<SieveHistory>(json);
        if (commandKind == QStringLiteral("deferred_send"))
            return deserialize<DeferredSendHistory>(json);
        if (commandKind == QStringLiteral("calendar_event"))
            return deserialize<CalendarEventHistory>(json);
        if (commandKind == QStringLiteral("calendar_preference"))
            return deserialize<CalendarPreferenceHistory>(json);
        if (commandKind == QStringLiteral("contact_card"))
            return deserialize<ContactCardHistory>(json);
        if (commandKind == QStringLiteral("address_book"))
            return deserialize<AddressBookHistory>(json);
        if (commandKind == QStringLiteral("contact_group"))
            return deserialize<ContactGroupHistory>(json);
        if (commandKind == QStringLiteral("impossible"))
            return deserialize<ImpossibleHistory>(json);

        return HistorySerializationError{
            .message = QStringLiteral("Unknown history command kind: ") + commandKind,
        };
    }

    QString payloadCommandKind(const HistoryPayload& payload)
    {
        return std::visit(
            [](const auto& value) -> QString
            {
                using Payload = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Payload, MailPatchHistory>)
                    return QStringLiteral("mail_patch");
                if constexpr (std::is_same_v<Payload, DraftHistory>)
                    return QStringLiteral("draft");
                if constexpr (std::is_same_v<Payload, SieveHistory>)
                    return QStringLiteral("sieve");
                if constexpr (std::is_same_v<Payload, DeferredSendHistory>)
                    return QStringLiteral("deferred_send");
                if constexpr (std::is_same_v<Payload, CalendarEventHistory>)
                    return QStringLiteral("calendar_event");
                if constexpr (std::is_same_v<Payload, CalendarPreferenceHistory>)
                    return QStringLiteral("calendar_preference");
                if constexpr (std::is_same_v<Payload, ContactCardHistory>)
                    return QStringLiteral("contact_card");
                if constexpr (std::is_same_v<Payload, AddressBookHistory>)
                    return QStringLiteral("address_book");
                if constexpr (std::is_same_v<Payload, ContactGroupHistory>)
                    return QStringLiteral("contact_group");
                if constexpr (std::is_same_v<Payload, ImpossibleHistory>)
                    return QStringLiteral("impossible");
            },
            payload);
    }

} // namespace javelin::app::undo
