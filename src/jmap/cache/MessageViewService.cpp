#include "jmap/cache/MessageViewService.h"

#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MimeMessageParser.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/render/HtmlMessageDocumentBuilder.h"

#include <QTextDocument>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] std::string defaultLanguageModelPath()
        {
#ifdef JAVELIN_FASTTEXT_LANGUAGE_MODEL_PATH
            return JAVELIN_FASTTEXT_LANGUAGE_MODEL_PATH;
#else
            return {};
#endif
        }

        [[nodiscard]] std::string detectionText(const MessageViewSnapshot& snapshot)
        {
            if (snapshot.plainTextBody.has_value() && !snapshot.plainTextBody->isTruncated)
            {
                return snapshot.plainTextBody->value;
            }

            if (snapshot.htmlBody.has_value() && !snapshot.htmlBody->isTruncated)
            {
                QTextDocument document;
                document.setHtml(QString::fromStdString(snapshot.htmlBody->value));
                return document.toPlainText().toStdString();
            }

            return {};
        }
    } // namespace

    MessageViewService::MessageViewService(DatabaseConnection& connection)
        : MessageViewService(connection, defaultLanguageModelPath())
    {
    }

    MessageViewService::MessageViewService(DatabaseConnection& connection,
                                           std::string languageModelPath)
        : m_connection(connection), m_languageDetectionService(std::move(languageModelPath))
    {
    }

    std::variant<std::optional<MessageViewSnapshot>, DatabaseError>
    MessageViewService::load(const std::string_view accountId, const std::string_view emailId) const
    {
        EmailRepository emailRepository{m_connection};
        RawMessageSourceRepository sourceRepository{m_connection};

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
        MessageViewSnapshot snapshot{
            .email = *email,
            .plainTextBody = parsed.plainTextBody,
            .htmlBody = parsed.htmlBody,
            .htmlRenderDocument = std::nullopt,
            .languageDetection = std::nullopt,
            .shouldOfferTranslation = false,
            .attachments = parsed.attachments,
        };

        if (snapshot.htmlBody.has_value())
        {
            javelin::jmap::render::HtmlMessageDocumentBuilder builder;
            snapshot.htmlRenderDocument =
                builder.build(accountId, emailId, snapshot.htmlBody->value, parsed.renderParts);
        }

        const auto textForDetection = detectionText(snapshot);
        if (!textForDetection.empty())
        {
            snapshot.languageDetection = m_languageDetectionService.detect(textForDetection);
            snapshot.shouldOfferTranslation =
                snapshot.languageDetection.has_value() &&
                javelin::jmap::language::shouldOfferTranslation(*snapshot.languageDetection);
            // TODO: Add TranslationService and translated-body state after provider selection.
        }

        return std::optional<MessageViewSnapshot>{std::move(snapshot)};
    }

} // namespace javelin::jmap::cache
