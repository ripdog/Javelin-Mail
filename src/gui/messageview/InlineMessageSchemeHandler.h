#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include <QByteArray>
#include <QWebEngineUrlSchemeHandler>

#include <optional>

namespace javelin::app
{

    class InlineMessageSchemeHandler : public QWebEngineUrlSchemeHandler
    {
      public:
        explicit InlineMessageSchemeHandler(
            const javelin::jmap::cache::ReadOnlyDatabaseConnection& connection,
            QObject* parent = nullptr);

        void requestStarted(QWebEngineUrlRequestJob* job) override;

      private:
        struct ReplyPayload
        {
            QByteArray mimeType;
            QByteArray body;
        };

        [[nodiscard]] std::optional<ReplyPayload> buildReply(const QUrl& url) const;
        [[nodiscard]] static QByteArray unavailableInlineImageSvg(const QString& label);

        const javelin::jmap::cache::ReadOnlyDatabaseConnection& m_connection;
    };

    void registerInlineMessageUrlScheme();

} // namespace javelin::app
