#include "jmap/render/HtmlMessageDocumentBuilder.h"
#include "jmap/render/InlineMessageUrl.h"

#include <QRegularExpression>

#include <unordered_map>

namespace javelin::jmap::render
{
    namespace
    {

        [[nodiscard]] std::string normalizeCid(std::string_view cid)
        {
            std::string normalized{cid};
            if (!normalized.empty() && normalized.front() == '<')
            {
                normalized.erase(normalized.begin());
            }
            if (!normalized.empty() && normalized.back() == '>')
            {
                normalized.pop_back();
            }
            return normalized;
        }

        [[nodiscard]] bool isRemoteUrl(const QString& value)
        {
            return value.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) ||
                   value.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) ||
                   value.startsWith(QStringLiteral("//"));
        }

        [[nodiscard]] QString escapeHtmlAttribute(const QString& value)
        {
            QString escaped = value;
            escaped.replace(QLatin1String("&"), QStringLiteral("&amp;"));
            escaped.replace(QLatin1String("\""), QStringLiteral("&quot;"));
            escaped.replace(QLatin1String("<"), QStringLiteral("&lt;"));
            escaped.replace(QLatin1String(">"), QStringLiteral("&gt;"));
            return escaped;
        }

        [[nodiscard]] QString blockRemoteCssUrls(const QString& style, bool& changed)
        {
            QString blocked = style;
            const QRegularExpression remoteCssUrl{
                QStringLiteral("(?i)url\\((['\"]?)(https?:)?//[^)'\"]+\\1\\)")};
            auto iterator = remoteCssUrl.globalMatch(style);
            changed = iterator.hasNext();
            blocked.replace(remoteCssUrl, QStringLiteral("url(about:blank)"));
            return blocked;
        }

    } // namespace

    HtmlRenderDocument HtmlMessageDocumentBuilder::build(
        const std::string_view accountId, const std::string_view emailId,
        const std::string_view sourceHtml,
        const std::vector<javelin::jmap::cache::EmailPart>& parts) const
    {
        std::unordered_map<std::string, std::string> inlineUrlsByCid;
        inlineUrlsByCid.reserve(parts.size());
        for (const auto& part : parts)
        {
            if (!part.isInlineRenderable || !part.cid.has_value())
            {
                continue;
            }

            const auto url = makeInlinePartUrl(accountId, emailId, part);
            if (!url.has_value())
            {
                continue;
            }

            inlineUrlsByCid.emplace(normalizeCid(*part.cid), *url);
        }

        QString html =
            QString::fromUtf8(sourceHtml.data(), static_cast<qsizetype>(sourceHtml.size()));
        html.remove(QRegularExpression{QStringLiteral("(?is)<script\\b[^>]*>.*?</script>")});
        html.replace(QRegularExpression{QStringLiteral(
                         "(?i)\\s+on[a-z0-9_-]+\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)")},
                     QString{});

        std::size_t inlineResourceCount = 0;
        std::size_t blockedRemoteResourceCount = 0;
        QRegularExpression attributePattern{
            QStringLiteral("(?i)\\b(src|poster|background)\\s*=\\s*(\"([^\"]*)\"|'([^']*)')")};

        qsizetype offset = 0;
        while (true)
        {
            const auto match = attributePattern.match(html, offset);
            if (!match.hasMatch())
            {
                break;
            }

            const auto attributeName = match.captured(1);
            const auto attributeText = match.captured(0);
            const auto attributeValue =
                match.captured(3).isEmpty() ? match.captured(4) : match.captured(3);

            QString replacement = attributeText;
            if (attributeValue.startsWith(QStringLiteral("cid:"), Qt::CaseInsensitive))
            {
                const auto cid = normalizeCid(attributeValue.sliced(4).toStdString());
                const auto it = inlineUrlsByCid.find(cid);
                if (it != inlineUrlsByCid.end())
                {
                    ++inlineResourceCount;
                    replacement = QStringLiteral("%1=\"%2\"")
                                      .arg(attributeName, QString::fromStdString(it->second));
                }
            }
            else if (isRemoteUrl(attributeValue))
            {
                ++blockedRemoteResourceCount;
                replacement = QStringLiteral("%1=\"about:blank\" data-javelin-blocked-src=\"%2\" "
                                             "data-javelin-remote-attr=\"%3\"")
                                  .arg(attributeName, escapeHtmlAttribute(attributeValue),
                                       attributeName.toLower());
            }

            html.replace(match.capturedStart(0), match.capturedLength(0), replacement);
            offset = match.capturedStart(0) + replacement.size();
        }

        QRegularExpression stylePattern{
            QStringLiteral("(?i)\\bstyle\\s*=\\s*(\"([^\"]*)\"|'([^']*)')")};
        offset = 0;
        while (true)
        {
            const auto match = stylePattern.match(html, offset);
            if (!match.hasMatch())
            {
                break;
            }

            const auto originalStyle =
                match.captured(2).isEmpty() ? match.captured(3) : match.captured(2);
            bool changed = false;
            const auto blockedStyle = blockRemoteCssUrls(originalStyle, changed);
            if (!changed)
            {
                offset = match.capturedEnd(0);
                continue;
            }

            ++blockedRemoteResourceCount;
            const auto replacement =
                QStringLiteral("style=\"%1\" data-javelin-blocked-style=\"%2\" "
                               "data-javelin-disabled-style=\"%3\"")
                    .arg(escapeHtmlAttribute(blockedStyle), escapeHtmlAttribute(originalStyle),
                         escapeHtmlAttribute(blockedStyle));
            html.replace(match.capturedStart(0), match.capturedLength(0), replacement);
            offset = match.capturedStart(0) + replacement.size();
        }

        html.replace(
            QRegularExpression{QStringLiteral("(?i)url\\((['\"]?)(https?:)?//[^)'\"]+\\1\\)")},
            QStringLiteral("url(about:blank)"));

        return HtmlRenderDocument{
            .html =
                QStringLiteral("<!doctype html><html><head><meta charset=\"utf-8\">"
                               "<meta http-equiv=\"Content-Security-Policy\" "
                               "content=\"default-src 'none'; img-src data: about: http: https: "
                               "javelin-message-inline:; media-src data: about: http: https: "
                               "javelin-message-inline:; "
                               "style-src 'unsafe-inline'; font-src data:;\">"
                               "</head><body>%1</body></html>")
                    .arg(html)
                    .toStdString(),
            .inlineResourceCount = inlineResourceCount,
            .blockedRemoteResourceCount = blockedRemoteResourceCount,
        };
    }

    std::optional<std::string>
    HtmlMessageDocumentBuilder::makeInlinePartUrl(const std::string_view accountId,
                                                  const std::string_view emailId,
                                                  const javelin::jmap::cache::EmailPart& part)
    {
        if (!part.blobId.has_value())
        {
            return std::nullopt;
        }

        return buildInlineMessageUrl(accountId, emailId, part.partId, *part.blobId);
    }

} // namespace javelin::jmap::render
