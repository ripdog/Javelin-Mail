#include "jmap/cache/MessageViewService.h"

#include "jmap/cache/EmailReadRepository.h"
#include "jmap/cache/MimeMessageParser.h"
#include "jmap/cache/RawMessageSourceReadRepository.h"
#include "jmap/render/HtmlMessageDocumentBuilder.h"

#include <QtConcurrentRun>

namespace javelin::jmap::cache
{
    MessageViewService::MessageViewService(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MessageViewService::MessageViewService(ReadOnlyDatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MessageViewResult MessageViewService::load(const std::string_view accountId,
                                               const std::string_view emailId) const
    {
        EmailReadRepository emailRepository{m_connection};
        RawMessageSourceReadRepository sourceRepository{m_connection};

        const auto emailResult = emailRepository.find(accountId, emailId);
        if (const auto* error = std::get_if<DatabaseError>(&emailResult))
        {
            return *error;
        }

        const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
        if (!email.has_value())
        {
            return std::optional<MessageViewSnapshot>{std::nullopt};
        }

        const auto sourceResult = sourceRepository.find(accountId, emailId);
        if (const auto* error = std::get_if<DatabaseError>(&sourceResult))
        {
            return *error;
        }

        const auto& source = std::get<std::optional<RawMessageSource>>(sourceResult);
        if (!source.has_value() || source->blobId != email->blobId)
        {
            return std::optional<MessageViewSnapshot>{std::nullopt};
        }

        const auto parsed = parseMessageSource(emailId, source->payload);
        auto plainTextBody = parsed.plainTextBody;
        if (!plainTextBody.has_value() && !parsed.htmlBody.has_value())
        {
            plainTextBody = MessageBody{
                .kind = MessageBodyKind::PlainText,
                .partId = {},
                .isTruncated = false,
                .value = "No content in email body.",
            };
        }
        MessageViewSnapshot snapshot{
            .email = *email,
            .plainTextBody = std::move(plainTextBody),
            .htmlBody = parsed.htmlBody,
            .htmlRenderDocument = std::nullopt,
            .unsubscribeUrl = parsed.unsubscribeUrl,
            .attachments = parsed.attachments,
        };

        if (snapshot.htmlBody.has_value())
        {
            javelin::jmap::render::HtmlMessageDocumentBuilder builder;
            snapshot.htmlRenderDocument =
                builder.build(accountId, emailId, snapshot.htmlBody->value, parsed.renderParts);
        }

        return std::optional<MessageViewSnapshot>{std::move(snapshot)};
    }

    QFuture<MessageViewResult> MessageViewService::loadAsync(std::string accountId,
                                                             std::string emailId) const
    {
        const QString databasePath = m_connection.database().databaseName();
        return QtConcurrent::run(
            [databasePath, accountId = std::move(accountId), emailId = std::move(emailId)]() mutable
            {
                ReadOnlyThreadConnectionFactory factory{
                    {.connectionNamePrefix = QStringLiteral("message-view"),
                     .databasePath = databasePath}};
                auto opened = factory.openForCurrentThread(accountId);
                if (const auto* error = std::get_if<DatabaseError>(&opened))
                    return MessageViewResult{*error};
                auto connection = std::get<ReadOnlyDatabaseConnection>(std::move(opened));
                return MessageViewService{connection}.load(accountId, emailId);
            });
    }

} // namespace javelin::jmap::cache
