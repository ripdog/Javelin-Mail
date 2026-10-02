#include "jmap/cache/MessageAuthActionDetector.h"

#include "jmap/render/HtmlTextExtractor.h"

#include <QRegularExpression>
#include <QStringView>
#include <QUrl>

#include <algorithm>
#include <optional>
#include <ranges>
#include <utility>

namespace javelin::jmap::cache
{
    namespace
    {
        constexpr qsizetype MaximumBodyCharacters = 100'000;
        constexpr qsizetype MaximumHtmlCharacters = 250'000;
        constexpr qsizetype CodeContextRadius = 90;
        constexpr int CodeThreshold = 5;
        constexpr int LinkThreshold = 6;

        struct ScoredCandidate
        {
            MessageAuthAction action;
            QString diagnostic;
        };

        [[nodiscard]] bool isZeroWidth(const QChar character)
        {
            const auto value = character.unicode();
            return value == 0x200b || value == 0x200c || value == 0x200d || value == 0x2060 ||
                   value == 0xfeff;
        }

        [[nodiscard]] QString normalizedText(QString text)
        {
            QString normalized;
            normalized.reserve(text.size());
            for (const auto character : text)
            {
                if (isZeroWidth(character))
                    continue;

                const auto digit = character.digitValue();
                if (digit >= 0)
                {
                    normalized.append(QChar{static_cast<char16_t>(u'0' + digit)});
                    continue;
                }

                if (character.unicode() == 0x00a0)
                {
                    normalized.append(QLatin1Char(' '));
                    continue;
                }

                normalized.append(character);
            }
            return normalized;
        }

        [[nodiscard]] QString clipped(QString text, const qsizetype maximum)
        {
            if (text.size() > maximum)
                text.truncate(maximum);
            return text;
        }

        [[nodiscard]] bool containsAny(const QString& text,
                                       const std::initializer_list<QStringView> terms)
        {
            return std::ranges::any_of(terms, [&text](const QStringView term)
                                       { return text.contains(term, Qt::CaseInsensitive); });
        }

        [[nodiscard]] bool containsWholeTerm(const QString& text, const QStringView term)
        {
            qsizetype from = 0;
            while (from < text.size())
            {
                const auto start = text.indexOf(term, from, Qt::CaseInsensitive);
                if (start < 0)
                    return false;

                const auto end = start + term.size();
                const bool startsAtBoundary = start == 0 || !text.at(start - 1).isLetterOrNumber();
                const bool endsAtBoundary = end == text.size() || !text.at(end).isLetterOrNumber();
                if (startsAtBoundary && endsAtBoundary)
                    return true;

                from = start + 1;
            }
            return false;
        }

        [[nodiscard]] bool containsAnyWholeTerm(const QString& text,
                                                const std::initializer_list<QStringView> terms)
        {
            return std::ranges::any_of(terms, [&text](const QStringView term)
                                       { return containsWholeTerm(text, term); });
        }

        [[nodiscard]] bool hasStrongAuthContext(const QString& text)
        {
            return containsAnyWholeTerm(
                text, {QStringView{u"verification code"}, QStringView{u"security code"},
                       QStringView{u"login code"}, QStringView{u"sign in code"},
                       QStringView{u"sign-in code"}, QStringView{u"one time code"},
                       QStringView{u"one-time code"}, QStringView{u"one time password"},
                       QStringView{u"one-time password"}, QStringView{u"authentication code"},
                       QStringView{u"passcode"}, QStringView{u"pin"}, QStringView{u"2fa"},
                       QStringView{u"two factor"}, QStringView{u"two-factor"}, QStringView{u"otp"},
                       QStringView{u"verify your"}, QStringView{u"confirm your"}});
        }

        [[nodiscard]] bool hasGenericCodeContext(const QString& text)
        {
            return containsWholeTerm(text, QStringView{u"code"});
        }

        [[nodiscard]] bool hasNegativeNumberContext(const QString& text)
        {
            return containsAny(
                text, {QStringView{u"order"}, QStringView{u"invoice"}, QStringView{u"tracking"},
                       QStringView{u"receipt"}, QStringView{u"reference"}, QStringView{u"booking"},
                       QStringView{u"flight"}, QStringView{u"voucher"}, QStringView{u"coupon"},
                       QStringView{u"total"}, QStringView{u"price"}, QStringView{u"amount"},
                       QStringView{u"customer number"}, QStringView{u"account number"}});
        }

        [[nodiscard]] QString contextAround(const QString& text, const qsizetype start,
                                            const qsizetype length, const qsizetype radius)
        {
            const auto contextStart = std::max<qsizetype>(0, start - radius);
            const auto contextEnd = std::min<qsizetype>(text.size(), start + length + radius);
            return text.mid(contextStart, contextEnd - contextStart);
        }

        [[nodiscard]] bool overlaps(const qsizetype firstStart, const qsizetype firstLength,
                                    const qsizetype secondStart, const qsizetype secondLength)
        {
            return firstStart < secondStart + secondLength &&
                   secondStart < firstStart + firstLength;
        }

        [[nodiscard]] bool isInsideDate(const QString& text, const qsizetype start,
                                        const qsizetype length)
        {
            static const QRegularExpression datePattern{
                QStringLiteral(R"(\b\d{1,4}[-/.]\d{1,2}[-/.]\d{1,4}\b)")};

            const auto contextStart = std::max<qsizetype>(0, start - 12);
            const auto context = text.mid(contextStart, length + 24);
            auto matches = datePattern.globalMatch(context);
            while (matches.hasNext())
            {
                const auto match = matches.next();
                if (overlaps(start, length, contextStart + match.capturedStart(),
                             match.capturedLength()))
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] int digitCount(const QStringView value)
        {
            return static_cast<int>(std::ranges::count_if(value, [](const QChar character)
                                                          { return character.isDigit(); }));
        }

        [[nodiscard]] bool isInsideTimeOfDay(const QString& text, const qsizetype start,
                                             const qsizetype length)
        {
            static const QRegularExpression timePattern{
                QStringLiteral(R"(\b(?:[01]?\d|2[0-3]):[0-5]\d\s*(?:[AaPp][Mm])?\b)")};

            const auto contextStart = std::max<qsizetype>(0, start - 8);
            const auto context = text.mid(contextStart, length + 16);
            auto matches = timePattern.globalMatch(context);
            while (matches.hasNext())
            {
                const auto match = matches.next();
                if (overlaps(start, length, contextStart + match.capturedStart(),
                             match.capturedLength()))
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool isInsidePhoneNumber(const QString& text, const qsizetype start,
                                               const qsizetype length)
        {
            static const QRegularExpression phonePattern{
                QStringLiteral(R"((?:\+?\d[\d ().-]{7,}\d))")};

            const auto contextStart = std::max<qsizetype>(0, start - 24);
            const auto context = text.mid(contextStart, length + 48);
            auto matches = phonePattern.globalMatch(context);
            while (matches.hasNext())
            {
                const auto match = matches.next();
                if (digitCount(match.capturedView()) < 9)
                    continue;
                if (overlaps(start, length, contextStart + match.capturedStart(),
                             match.capturedLength()))
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool isInsideUrlOrEmailToken(const QString& text, const qsizetype start)
        {
            auto tokenStart = start;
            while (tokenStart > 0 && !text.at(tokenStart - 1).isSpace())
                --tokenStart;

            auto tokenEnd = start;
            while (tokenEnd < text.size() && !text.at(tokenEnd).isSpace())
                ++tokenEnd;

            const auto token = QStringView{text}.sliced(tokenStart, tokenEnd - tokenStart);
            return token.contains(QLatin1StringView{"://"}) || token.contains(QLatin1Char('@'));
        }

        [[nodiscard]] bool hasCurrencyOrDecimalContext(const QString& text, const qsizetype start,
                                                       const qsizetype length)
        {
            const auto previous = start > 0 ? text.at(start - 1) : QChar{};
            const auto previousTwo = start > 1 ? text.at(start - 2) : QChar{};
            const auto next = start + length < text.size() ? text.at(start + length) : QChar{};
            const auto nextTwo =
                start + length + 1 < text.size() ? text.at(start + length + 1) : QChar{};

            const auto isCurrency = [](const QChar character)
            {
                return character == QLatin1Char('$') || character.unicode() == 0x20ac ||
                       character.unicode() == 0x00a3 || character.unicode() == 0x00a5;
            };

            if (isCurrency(previous) || (previous.isSpace() && isCurrency(previousTwo)))
                return true;
            if ((previous == QLatin1Char('.') && previousTwo.isDigit()) ||
                (next == QLatin1Char('.') && nextTwo.isDigit()))
            {
                return true;
            }
            return false;
        }

        [[nodiscard]] QString compactCode(QString code)
        {
            code.remove(QRegularExpression{QStringLiteral(R"(\s+)")});
            return code;
        }

        [[nodiscard]] QString maskedCode(const QString& code)
        {
            const auto alnumCount = std::ranges::count_if(code, [](const QChar character)
                                                          { return character.isLetterOrNumber(); });
            return QStringLiteral("<%1-character code>").arg(alnumCount);
        }

        [[nodiscard]] std::optional<ScoredCandidate>
        scoreCode(const QString& body, const QString& subject, const QRegularExpressionMatch& match)
        {
            const auto raw = match.captured(1);
            const auto value = compactCode(raw);
            const auto start = match.capturedStart(1);
            const auto length = match.capturedLength(1);

            const auto digits = digitCount(QStringView{value});
            if (digits == 0)
                return std::nullopt;

            const auto letters = static_cast<int>(std::ranges::count_if(
                value, [](const QChar character) { return character.isLetter(); }));
            const auto alnum = digits + letters;
            if (alnum < 4 || alnum > 10)
                return std::nullopt;

            QStringList reasons;
            int score = 0;

            const bool numeric = letters == 0;
            if (numeric && alnum == 4)
            {
                bool ok = false;
                const auto year = value.toInt(&ok);
                if (ok && year >= 1900 && year <= 2099)
                {
                    return ScoredCandidate{
                        .action = {.kind = MessageAuthActionKind::CopyCode,
                                   .value = value,
                                   .score = -20},
                        .diagnostic =
                            QStringLiteral("code candidate %1 rejected: looks like a year")
                                .arg(maskedCode(value)),
                    };
                }
            }

            if (isInsideDate(body, start, length))
            {
                return ScoredCandidate{
                    .action = {.kind = MessageAuthActionKind::CopyCode,
                               .value = value,
                               .score = -20},
                    .diagnostic = QStringLiteral("code candidate %1 rejected: part of a date")
                                      .arg(maskedCode(value)),
                };
            }
            if (isInsideTimeOfDay(body, start, length))
            {
                return ScoredCandidate{
                    .action = {.kind = MessageAuthActionKind::CopyCode,
                               .value = value,
                               .score = -20},
                    .diagnostic = QStringLiteral("code candidate %1 rejected: part of a time")
                                      .arg(maskedCode(value)),
                };
            }
            if (isInsidePhoneNumber(body, start, length))
            {
                return ScoredCandidate{
                    .action = {.kind = MessageAuthActionKind::CopyCode,
                               .value = value,
                               .score = -20},
                    .diagnostic =
                        QStringLiteral("code candidate %1 rejected: part of a phone number")
                            .arg(maskedCode(value)),
                };
            }
            if (isInsideUrlOrEmailToken(body, start))
            {
                return ScoredCandidate{
                    .action = {.kind = MessageAuthActionKind::CopyCode,
                               .value = value,
                               .score = -20},
                    .diagnostic =
                        QStringLiteral("code candidate %1 rejected: inside URL/email token")
                            .arg(maskedCode(value)),
                };
            }
            if (hasCurrencyOrDecimalContext(body, start, length))
            {
                return ScoredCandidate{
                    .action = {.kind = MessageAuthActionKind::CopyCode,
                               .value = value,
                               .score = -20},
                    .diagnostic = QStringLiteral("code candidate %1 rejected: numeric amount")
                                      .arg(maskedCode(value)),
                };
            }

            if (numeric && alnum == 6)
            {
                score += 3;
                reasons.push_back(QStringLiteral("+3 six-digit shape"));
            }
            else
            {
                score += 2;
                reasons.push_back(QStringLiteral("+2 OTP-like shape"));
            }

            const auto context = contextAround(body, start, length, CodeContextRadius);
            const bool strongLocal = hasStrongAuthContext(context);
            const bool strongSubject = hasStrongAuthContext(subject);
            const bool genericLocal = hasGenericCodeContext(context);
            const bool genericSubject = hasGenericCodeContext(subject);

            if (strongLocal)
            {
                score += 5;
                reasons.push_back(QStringLiteral("+5 nearby authentication wording"));
            }
            else if (genericLocal)
            {
                score += 2;
                reasons.push_back(QStringLiteral("+2 nearby code/PIN wording"));
            }

            if (strongSubject)
            {
                score += 4;
                reasons.push_back(QStringLiteral("+4 authentication subject"));
            }
            else if (genericSubject)
            {
                score += 1;
                reasons.push_back(QStringLiteral("+1 code/PIN subject"));
            }

            if (hasNegativeNumberContext(context))
            {
                score -= 5;
                reasons.push_back(QStringLiteral("-5 order/reference/amount context"));
            }

            if (!strongLocal && !strongSubject && !genericLocal && !genericSubject)
            {
                score -= 4;
                reasons.push_back(QStringLiteral("-4 no authentication context"));
            }

            if (letters >= 3 && value == value.toLower() && !strongLocal && !strongSubject)
            {
                score -= 3;
                reasons.push_back(QStringLiteral("-3 lowercase word-like candidate"));
            }

            return ScoredCandidate{
                .action = {.kind = MessageAuthActionKind::CopyCode, .value = value, .score = score},
                .diagnostic = QStringLiteral("code candidate %1 score=%2: %3")
                                  .arg(maskedCode(value), QString::number(score),
                                       reasons.join(QStringLiteral(", "))),
            };
        }

        [[nodiscard]] QString decodeHref(QString value)
        {
            value.replace(QStringLiteral("&amp;"), QStringLiteral("&"), Qt::CaseInsensitive);
            value.replace(QStringLiteral("&#38;"), QStringLiteral("&"), Qt::CaseInsensitive);
            value.replace(QStringLiteral("&#x26;"), QStringLiteral("&"), Qt::CaseInsensitive);
            value.replace(QStringLiteral("&quot;"), QStringLiteral("\""), Qt::CaseInsensitive);
            return value.trimmed();
        }

        [[nodiscard]] bool hasStrongLinkText(const QString& text)
        {
            return containsAnyWholeTerm(
                text, {QStringView{u"sign in"}, QStringView{u"sign-in"}, QStringView{u"log in"},
                       QStringView{u"login"}, QStringView{u"authenticate"},
                       QStringView{u"access account"}, QStringView{u"magic link"}});
        }

        [[nodiscard]] bool hasAmbiguousAuthLinkText(const QString& text)
        {
            return containsAnyWholeTerm(text, {QStringView{u"verify"}, QStringView{u"confirm"}});
        }

        [[nodiscard]] bool hasWeakLinkText(const QString& text)
        {
            return containsAny(text, {QStringView{u"continue"}, QStringView{u"open account"}});
        }

        [[nodiscard]] bool hasNegativeLinkText(const QString& text)
        {
            return containsAny(text, {QStringView{u"unsubscribe"}, QStringView{u"privacy"},
                                      QStringView{u"terms"}, QStringView{u"preferences"},
                                      QStringView{u"view in browser"}, QStringView{u"view online"},
                                      QStringView{u"help"}, QStringView{u"support"},
                                      QStringView{u"facebook"}, QStringView{u"instagram"},
                                      QStringView{u"twitter"}, QStringView{u"linkedin"}});
        }

        [[nodiscard]] bool urlLooksAuthRelated(const QUrl& url)
        {
            return containsAny(url.path(), {QStringView{u"verify"}, QStringView{u"auth"},
                                            QStringView{u"login"}, QStringView{u"signin"},
                                            QStringView{u"sign-in"}, QStringView{u"magic"},
                                            QStringView{u"confirm"}, QStringView{u"activate"}});
        }

        [[nodiscard]] bool urlHasAuthParameter(const QUrl& url)
        {
            return containsAny(url.query(),
                               {QStringView{u"token="}, QStringView{u"code="}, QStringView{u"otp="},
                                QStringView{u"verify="}, QStringView{u"verification="}});
        }

        [[nodiscard]] std::optional<ScoredCandidate>
        scoreLink(const QString& html, const QString& subject, const QRegularExpressionMatch& match)
        {
            static const QRegularExpression hrefPattern{
                QStringLiteral("\\bhref\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))"),
                QRegularExpression::CaseInsensitiveOption};

            const auto attributes = match.captured(1);
            const auto hrefMatch = hrefPattern.match(attributes);
            if (!hrefMatch.hasMatch())
                return std::nullopt;

            QString href;
            for (int capture = 1; capture <= 3; ++capture)
            {
                if (!hrefMatch.captured(capture).isEmpty())
                {
                    href = hrefMatch.captured(capture);
                    break;
                }
            }
            href = decodeHref(std::move(href));
            const auto url = QUrl::fromEncoded(href.toUtf8());
            if (!url.isValid() ||
                (url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 &&
                 url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0) ||
                url.host().isEmpty())
            {
                return std::nullopt;
            }

            const auto anchorText =
                javelin::jmap::render::plainTextFromHtml(match.captured(2)).simplified();
            if (hasNegativeLinkText(anchorText))
            {
                return ScoredCandidate{
                    .action = {.kind = MessageAuthActionKind::OpenLink,
                               .value = QString::fromUtf8(url.toEncoded()),
                               .score = -20},
                    .diagnostic =
                        QStringLiteral("link candidate host=%1 rejected: negative link text")
                            .arg(url.host()),
                };
            }

            const auto contextStart = std::max<qsizetype>(0, match.capturedStart() - 240);
            const auto contextEnd = std::min<qsizetype>(html.size(), match.capturedEnd() + 240);
            const auto context = javelin::jmap::render::plainTextFromHtml(
                                     html.mid(contextStart, contextEnd - contextStart))
                                     .simplified();

            QStringList reasons;
            int score = 0;

            if (hasStrongLinkText(anchorText))
            {
                score += 6;
                reasons.push_back(QStringLiteral("+6 authentication link text"));
            }
            else if (hasAmbiguousAuthLinkText(anchorText))
            {
                score += 3;
                reasons.push_back(
                    QStringLiteral("+3 verify/confirm link text requiring corroboration"));
            }
            else if (hasWeakLinkText(anchorText))
            {
                score += 2;
                reasons.push_back(QStringLiteral("+2 weak action link text"));
            }

            if (hasStrongAuthContext(subject))
            {
                score += 3;
                reasons.push_back(QStringLiteral("+3 authentication subject"));
            }
            if (hasStrongAuthContext(context))
            {
                score += 3;
                reasons.push_back(QStringLiteral("+3 nearby authentication wording"));
            }
            if (urlLooksAuthRelated(url))
            {
                score += 2;
                reasons.push_back(QStringLiteral("+2 authentication-looking URL path"));
            }
            if (urlHasAuthParameter(url))
            {
                score += 1;
                reasons.push_back(QStringLiteral("+1 authentication-looking query parameter"));
            }

            if (hasNegativeLinkText(context) && !hasStrongLinkText(anchorText))
            {
                score -= 4;
                reasons.push_back(QStringLiteral("-4 footer/support context"));
            }

            return ScoredCandidate{
                .action = {.kind = MessageAuthActionKind::OpenLink,
                           .value = QString::fromUtf8(url.toEncoded()),
                           .score = score},
                .diagnostic = QStringLiteral("link candidate host=%1 score=%2: %3")
                                  .arg(url.host(), QString::number(score),
                                       reasons.isEmpty() ? QStringLiteral("no positive signals")
                                                         : reasons.join(QStringLiteral(", "))),
            };
        }

        void considerCandidate(const ScoredCandidate& candidate, const int threshold,
                               std::optional<MessageAuthAction>& best)
        {
            if (candidate.action.score < threshold)
                return;
            if (!best.has_value() || candidate.action.score > best->score)
                best = candidate.action;
        }
    } // namespace

    QString MessageAuthActionDetection::debugSummary() const
    {
        QStringList lines;
        if (action.has_value())
        {
            const auto kind = action->kind == MessageAuthActionKind::CopyCode
                                  ? QStringLiteral("copy-code")
                                  : QStringLiteral("open-link");
            lines.push_back(
                QStringLiteral("decision=%1 score=%2").arg(kind, QString::number(action->score)));
        }
        else
        {
            lines.push_back(QStringLiteral("decision=none"));
        }
        lines.append(diagnostics);
        return lines.join(QLatin1Char('\n'));
    }

    MessageAuthActionDetection
    MessageAuthActionDetector::detect(const MessageAuthActionInput& input) const
    {
        MessageAuthActionDetection result;

        const auto subject = normalizedText(input.subject).simplified();
        auto body = normalizedText(input.plainText);
        if (!input.html.isEmpty())
        {
            const auto htmlText = normalizedText(javelin::jmap::render::plainTextFromHtml(
                clipped(input.html, MaximumHtmlCharacters)));
            if (body.trimmed().isEmpty())
            {
                body = htmlText;
                result.diagnostics.push_back(QStringLiteral("body-source=html"));
            }
            else if (!htmlText.trimmed().isEmpty())
            {
                body += QLatin1Char('\n');
                body += htmlText;
                result.diagnostics.push_back(QStringLiteral("body-source=plain-text+html"));
            }
            else
            {
                result.diagnostics.push_back(QStringLiteral("body-source=plain-text"));
            }
        }
        else
        {
            result.diagnostics.push_back(QStringLiteral("body-source=plain-text"));
        }

        const bool bodyTruncated = body.size() > MaximumBodyCharacters;
        body = clipped(std::move(body), MaximumBodyCharacters);
        if (bodyTruncated)
            result.diagnostics.push_back(QStringLiteral("body-truncated=true"));

        std::optional<MessageAuthAction> bestCode;
        std::optional<MessageAuthAction> bestLink;
        int codeCandidates = 0;
        int linkCandidates = 0;

        static const QRegularExpression codePattern{QStringLiteral(
            R"((?<![A-Za-z0-9])((?:[A-Za-z]{1,2}-){1,2}[A-Za-z0-9]{3,8}|[0-9]{3}[ \x{00A0}][0-9]{3}|[A-Za-z0-9]{4,8})(?![A-Za-z0-9]))")};

        auto codeMatches = codePattern.globalMatch(body);
        while (codeMatches.hasNext())
        {
            const auto candidate = scoreCode(body, subject, codeMatches.next());
            if (!candidate.has_value())
                continue;
            ++codeCandidates;
            result.diagnostics.push_back(candidate->diagnostic);
            considerCandidate(*candidate, CodeThreshold, bestCode);
        }

        if (!input.html.isEmpty())
        {
            static const QRegularExpression anchorPattern{
                QStringLiteral(R"(<a\b([^>]*)>(.*?)</a\s*>)"),
                QRegularExpression::CaseInsensitiveOption |
                    QRegularExpression::DotMatchesEverythingOption};

            const auto html = clipped(normalizedText(input.html), MaximumHtmlCharacters);
            auto linkMatches = anchorPattern.globalMatch(html);
            while (linkMatches.hasNext())
            {
                const auto candidate = scoreLink(html, subject, linkMatches.next());
                if (!candidate.has_value())
                    continue;
                ++linkCandidates;
                result.diagnostics.push_back(candidate->diagnostic);
                considerCandidate(*candidate, LinkThreshold, bestLink);
            }
        }

        // Plain-first JMAP bodies can contain a usable URL without an HTML alternative.
        static const QRegularExpression urlPattern{QStringLiteral(R"(https?://[^\s<>"']+)"),
                                                   QRegularExpression::CaseInsensitiveOption};
        static const QRegularExpression plainAnchorPattern{
            QStringLiteral(R"(<a\b([^>]*)>(.*?)</a\s*>)"),
            QRegularExpression::DotMatchesEverythingOption};
        const auto plainText = clipped(normalizedText(input.plainText), MaximumBodyCharacters);
        auto urls = urlPattern.globalMatch(plainText);
        while (urls.hasNext())
        {
            const auto match = urls.next();
            auto url = match.captured();
            while (!url.isEmpty() && QStringView{u".,;!)]}"}.contains(url.back()))
                url.chop(1);
            const auto context =
                contextAround(plainText, match.capturedStart(), match.capturedLength(), 240);
            const auto html = context.toHtmlEscaped() +
                              QStringLiteral("<a href=\"%1\">%1</a>").arg(url.toHtmlEscaped());
            const auto candidate = scoreLink(html, subject, plainAnchorPattern.match(html));
            if (!candidate)
                continue;
            ++linkCandidates;
            result.diagnostics.push_back(candidate->diagnostic);
            considerCandidate(*candidate, LinkThreshold, bestLink);
        }

        result.action = bestCode ? std::move(bestCode) : std::move(bestLink);
        result.diagnostics.push_front(QStringLiteral("candidates: codes=%1 links=%2")
                                          .arg(codeCandidates)
                                          .arg(linkCandidates));
        return result;
    }

} // namespace javelin::jmap::cache
