#include "app/InlineMessageSchemeHandler.h"
#include "app/WebEngineSetup.h"

#include "jmap/cache/InlinePartPayloadRepository.h"
#include "jmap/cache/MessageContentRepository.h"
#include "jmap/render/InlineMessageUrl.h"

#include <QBuffer>
#include <QUrl>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>

#include <algorithm>

namespace javelin::app
{

    namespace
    {

        [[nodiscard]] bool hasRegisteredInlineScheme()
        {
            return QWebEngineUrlScheme::schemeByName(
                       javelin::jmap::render::inlineMessageUrlScheme().toUtf8())
                       .name()
                   .size() > 0;
        }

        [[nodiscard]] QString escapedSvgText(QString value)
        {
            value.replace('&', QStringLiteral("&amp;"));
            value.replace('<', QStringLiteral("&lt;"));
            value.replace('>', QStringLiteral("&gt;"));
            return value;
        }

    } // namespace

    InlineMessageSchemeHandler::InlineMessageSchemeHandler(
        javelin::jmap::cache::DatabaseConnection& connection, QObject* parent)
        : QWebEngineUrlSchemeHandler(parent), m_connection(connection)
    {
    }

    void InlineMessageSchemeHandler::requestStarted(QWebEngineUrlRequestJob* job)
    {
        const auto payload = buildReply(job->requestUrl());
        if (!payload.has_value())
        {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }

        auto* buffer = new QBuffer(job);
        buffer->setData(payload->body);
        buffer->open(QIODevice::ReadOnly);
        job->reply(payload->mimeType, buffer);
    }

    std::optional<InlineMessageSchemeHandler::ReplyPayload>
    InlineMessageSchemeHandler::buildReply(const QUrl& url) const
    {
        const auto parts = javelin::jmap::render::parseInlineMessageUrl(url);
        if (!parts.has_value())
        {
            return std::nullopt;
        }

        javelin::jmap::cache::MessageContentRepository repository{m_connection};
        const auto loadResult = repository.loadParts(parts->accountId, parts->emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&loadResult))
        {
            Q_UNUSED(error);
            return std::nullopt;
        }

        const auto& cachedParts =
            std::get<std::vector<javelin::jmap::cache::EmailPart>>(loadResult);
        const auto it = std::find_if(cachedParts.begin(), cachedParts.end(),
                                     [&parts](const auto& part)
                                     {
                                         return part.partId == parts->partId &&
                                                part.blobId == std::optional<std::string>{
                                                                   parts->blobId} &&
                                                part.isInlineRenderable;
                                     });
        if (it == cachedParts.end())
        {
            return std::nullopt;
        }

        javelin::jmap::cache::InlinePartPayloadRepository payloadRepository{m_connection};
        const auto payloadResult =
            payloadRepository.find(parts->accountId, parts->emailId, parts->partId);
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&payloadResult))
        {
            Q_UNUSED(error);
            return std::nullopt;
        }

        const auto& payload =
            std::get<std::optional<javelin::jmap::cache::InlinePartPayload>>(payloadResult);
        if (payload.has_value() && payload->blobId == parts->blobId)
        {
            return ReplyPayload{
                .mimeType = QByteArray::fromStdString(payload->mediaType),
                .body = payload->payload,
            };
        }

        if (it->mediaType.rfind("image/", 0) == 0)
        {
            const QString label =
                it->name.has_value() ? QString::fromStdString(*it->name)
                                     : QString::fromStdString(it->partId);
            return ReplyPayload{
                .mimeType = QByteArrayLiteral("image/svg+xml"),
                .body = unavailableInlineImageSvg(label),
            };
        }

        return std::nullopt;
    }

    QByteArray InlineMessageSchemeHandler::unavailableInlineImageSvg(const QString& label)
    {
        return QStringLiteral(
                   "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"480\" height=\"160\" "
                   "viewBox=\"0 0 480 160\">"
                   "<rect width=\"480\" height=\"160\" fill=\"#f4efe6\"/>"
                   "<rect x=\"16\" y=\"16\" width=\"448\" height=\"128\" rx=\"12\" fill=\"#e3d6c4\" "
                   "stroke=\"#8a6f54\" stroke-width=\"2\" stroke-dasharray=\"8 6\"/>"
                   "<text x=\"32\" y=\"68\" font-family=\"sans-serif\" font-size=\"18\" "
                   "fill=\"#5c4733\">Inline media is referenced by this message.</text>"
                   "<text x=\"32\" y=\"98\" font-family=\"sans-serif\" font-size=\"14\" "
                   "fill=\"#5c4733\">Cached payload is not available yet for %1.</text>"
                   "</svg>")
            .arg(escapedSvgText(label))
            .toUtf8();
    }

    void registerInlineMessageUrlScheme()
    {
        if (hasRegisteredInlineScheme())
        {
            return;
        }

        QWebEngineUrlScheme scheme{
            javelin::jmap::render::inlineMessageUrlScheme().toUtf8()};
        scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
        scheme.setFlags(QWebEngineUrlScheme::SecureScheme |
                        QWebEngineUrlScheme::LocalScheme |
                        QWebEngineUrlScheme::LocalAccessAllowed);
        QWebEngineUrlScheme::registerScheme(scheme);
    }

} // namespace javelin::app
