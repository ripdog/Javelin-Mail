#include "jmap/render/InlineMessageUrl.h"

#include <QUrlQuery>

namespace javelin::jmap::render
{

    QString inlineMessageUrlScheme()
    {
        return QStringLiteral("javelin-message-inline");
    }

    std::string buildInlineMessageUrl(const std::string_view accountId,
                                      const std::string_view emailId,
                                      const std::string_view partId,
                                      const std::string_view blobId)
    {
        QUrl url;
        url.setScheme(inlineMessageUrlScheme());
        url.setHost(QStringLiteral("message"));

        QUrlQuery query;
        query.addQueryItem(QStringLiteral("account"),
                           QString::fromStdString(std::string{accountId}));
        query.addQueryItem(QStringLiteral("email"),
                           QString::fromStdString(std::string{emailId}));
        query.addQueryItem(QStringLiteral("part"),
                           QString::fromStdString(std::string{partId}));
        query.addQueryItem(QStringLiteral("blob"),
                           QString::fromStdString(std::string{blobId}));
        url.setQuery(query);

        return url.toString(QUrl::FullyEncoded).toStdString();
    }

    std::optional<InlineMessageUrlParts> parseInlineMessageUrl(const QUrl& url)
    {
        if (url.scheme() != inlineMessageUrlScheme() || url.host() != QStringLiteral("message"))
        {
            return std::nullopt;
        }

        const QUrlQuery query{url};
        const auto accountId = query.queryItemValue(QStringLiteral("account"));
        const auto emailId = query.queryItemValue(QStringLiteral("email"));
        const auto partId = query.queryItemValue(QStringLiteral("part"));
        const auto blobId = query.queryItemValue(QStringLiteral("blob"));
        if (accountId.isEmpty() || emailId.isEmpty() || partId.isEmpty() || blobId.isEmpty())
        {
            return std::nullopt;
        }

        return InlineMessageUrlParts{
            .accountId = accountId.toStdString(),
            .emailId = emailId.toStdString(),
            .partId = partId.toStdString(),
            .blobId = blobId.toStdString(),
        };
    }

} // namespace javelin::jmap::render
