#include "jmap/render/HtmlTextExtractor.h"

#include <KCharsets>

#include <QChar>
#include <QStringView>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace javelin::jmap::render
{
    namespace
    {
        constexpr std::array blockTags{
            "address",  "article",    "aside",  "blockquote", "caption", "dd",  "div", "dl", "dt",
            "fieldset", "figcaption", "figure", "footer",     "form",    "h1",  "h2",  "h3", "h4",
            "h5",       "h6",         "header", "li",         "main",    "nav", "ol",  "p",  "pre",
            "section",  "table",      "tbody",  "tfoot",      "thead",   "tr",  "ul",
        };

        [[nodiscard]] bool contains(const auto& values, const QStringView value)
        {
            for (const auto candidate : values)
            {
                if (value.compare(QLatin1StringView{candidate}, Qt::CaseInsensitive) == 0)
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool isValidCodePoint(const char32_t value)
        {
            return value != 0 && value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff);
        }

        [[nodiscard]] std::optional<QString> decodedEntity(const QStringView entity)
        {
            if (entity.isEmpty())
                return std::nullopt;

            char32_t value = 0;
            if (entity.front() == QLatin1Char('#'))
            {
                bool ok = false;
                const bool hexadecimal = entity.size() > 2 && (entity.at(1) == QLatin1Char('x') ||
                                                               entity.at(1) == QLatin1Char('X'));
                const auto digits = hexadecimal ? entity.sliced(2) : entity.sliced(1);
                value = static_cast<char32_t>(digits.toUInt(&ok, hexadecimal ? 16 : 10));
                if (!ok || !isValidCodePoint(value))
                    return std::nullopt;
            }
            else
            {
                const auto character = KCharsets::fromEntity(entity);
                if (character.isNull())
                    return std::nullopt;
                return QString{character};
            }

            return QString::fromUcs4(&value, 1);
        }

        class TextBuilder final
        {
          public:
            void append(QStringView text, const bool preformatted)
            {
                qsizetype offset = 0;
                while (offset < text.size())
                {
                    if (text.at(offset) == QLatin1Char('&'))
                    {
                        const auto semicolon = text.indexOf(QLatin1Char(';'), offset + 1);
                        if (semicolon > offset && semicolon - offset <= maximumEntityLength)
                        {
                            const auto decoded =
                                decodedEntity(text.sliced(offset + 1, semicolon - offset - 1));
                            if (decoded.has_value())
                            {
                                appendDecoded(*decoded, preformatted);
                                offset = semicolon + 1;
                                continue;
                            }
                        }
                    }

                    appendCharacter(text.at(offset), preformatted);
                    ++offset;
                }
            }

            void blockBoundary()
            {
                trimTrailingHorizontalWhitespace();
                if (!m_text.isEmpty() && m_text.back() != QLatin1Char('\n'))
                    m_text.append(QLatin1Char('\n'));
            }

            void explicitLineBreak()
            {
                trimTrailingHorizontalWhitespace();
                if (!m_text.isEmpty())
                    m_text.append(QLatin1Char('\n'));
            }

            void tableCellBoundary()
            {
                trimTrailingHorizontalWhitespace();
                if (!m_text.isEmpty() && m_text.back() != QLatin1Char('\n') &&
                    m_text.back() != QLatin1Char('\t'))
                {
                    m_text.append(QLatin1Char('\t'));
                }
            }

            [[nodiscard]] QString take()
            {
                while (!m_text.isEmpty() && m_text.back().isSpace())
                    m_text.chop(1);
                while (!m_text.isEmpty() && m_text.front().isSpace())
                    m_text.remove(0, 1);
                return std::move(m_text);
            }

          private:
            static constexpr qsizetype maximumEntityLength = 32;

            void appendDecoded(const QString& decoded, const bool preformatted)
            {
                for (const auto character : decoded)
                    appendCharacter(character, preformatted);
            }

            void appendCharacter(const QChar character, const bool preformatted)
            {
                if (preformatted)
                {
                    if (character == QLatin1Char('\r'))
                        return;
                    m_text.append(character);
                    return;
                }

                if (character.isSpace())
                {
                    if (!m_text.isEmpty() && !m_text.back().isSpace())
                        m_text.append(QLatin1Char(' '));
                    return;
                }
                m_text.append(character);
            }

            void trimTrailingHorizontalWhitespace()
            {
                while (!m_text.isEmpty() &&
                       (m_text.back() == QLatin1Char(' ') || m_text.back() == QLatin1Char('\t')))
                {
                    m_text.chop(1);
                }
            }

            QString m_text;
        };

        [[nodiscard]] qsizetype tagEnd(const QString& html, const qsizetype start)
        {
            QChar quote;
            for (auto offset = start; offset < html.size(); ++offset)
            {
                const auto character = html.at(offset);
                if (!quote.isNull())
                {
                    if (character == quote)
                        quote = {};
                    continue;
                }
                if (character == QLatin1Char('\'') || character == QLatin1Char('"'))
                {
                    quote = character;
                    continue;
                }
                if (character == QLatin1Char('>'))
                    return offset;
            }
            return -1;
        }

        [[nodiscard]] QStringView tagName(QStringView contents)
        {
            contents = contents.trimmed();
            if (!contents.isEmpty() && contents.front() == QLatin1Char('/'))
                contents = contents.sliced(1).trimmed();
            qsizetype length = 0;
            while (length < contents.size())
            {
                const auto character = contents.at(length);
                if (!character.isLetterOrNumber() && character != QLatin1Char(':') &&
                    character != QLatin1Char('-'))
                {
                    break;
                }
                ++length;
            }
            return contents.first(length);
        }

        [[nodiscard]] bool isClosingTag(QStringView contents)
        {
            contents = contents.trimmed();
            return !contents.isEmpty() && contents.front() == QLatin1Char('/');
        }

        [[nodiscard]] qsizetype skipElement(const QString& html, const qsizetype bodyStart,
                                            const QStringView name)
        {
            const auto closing = QStringLiteral("</%1").arg(name);
            const auto closingStart = html.indexOf(closing, bodyStart, Qt::CaseInsensitive);
            if (closingStart < 0)
                return html.size();
            const auto closingEnd = tagEnd(html, closingStart + 2);
            return closingEnd < 0 ? html.size() : closingEnd + 1;
        }
    } // namespace

    QString plainTextFromHtml(QString html)
    {
        TextBuilder output;
        qsizetype offset = 0;
        int preformattedDepth = 0;

        while (offset < html.size())
        {
            const auto opening = html.indexOf(QLatin1Char('<'), offset);
            if (opening < 0)
            {
                output.append(QStringView{html}.sliced(offset), preformattedDepth > 0);
                break;
            }
            if (opening > offset)
                output.append(QStringView{html}.sliced(offset, opening - offset),
                              preformattedDepth > 0);

            if (QStringView{html}.sliced(opening).startsWith(QLatin1StringView{"<!--"}))
            {
                const auto commentEnd = html.indexOf(QStringLiteral("-->"), opening + 4);
                offset = commentEnd < 0 ? html.size() : commentEnd + 3;
                continue;
            }

            const auto end = tagEnd(html, opening + 1);
            if (end < 0)
            {
                output.append(QStringView{html}.sliced(opening), preformattedDepth > 0);
                break;
            }

            auto contents = QStringView{html}.sliced(opening + 1, end - opening - 1).trimmed();
            if (contents.isEmpty() || contents.front() == QLatin1Char('!') ||
                contents.front() == QLatin1Char('?'))
            {
                offset = end + 1;
                continue;
            }

            const auto name = tagName(contents);
            if (name.isEmpty())
            {
                output.append(QStringView{html}.sliced(opening, end - opening + 1),
                              preformattedDepth > 0);
                offset = end + 1;
                continue;
            }

            const bool closing = isClosingTag(contents);
            if (!closing && (name.compare(QLatin1StringView{"head"}, Qt::CaseInsensitive) == 0 ||
                             name.compare(QLatin1StringView{"script"}, Qt::CaseInsensitive) == 0 ||
                             name.compare(QLatin1StringView{"style"}, Qt::CaseInsensitive) == 0 ||
                             name.compare(QLatin1StringView{"template"}, Qt::CaseInsensitive) == 0))
            {
                offset = skipElement(html, end + 1, name);
                continue;
            }

            if (name.compare(QLatin1StringView{"br"}, Qt::CaseInsensitive) == 0 ||
                name.compare(QLatin1StringView{"hr"}, Qt::CaseInsensitive) == 0)
            {
                output.explicitLineBreak();
            }
            else if (name.compare(QLatin1StringView{"td"}, Qt::CaseInsensitive) == 0 ||
                     name.compare(QLatin1StringView{"th"}, Qt::CaseInsensitive) == 0)
            {
                if (closing)
                    output.tableCellBoundary();
            }
            else if (contains(blockTags, name))
            {
                output.blockBoundary();
            }

            if (name.compare(QLatin1StringView{"pre"}, Qt::CaseInsensitive) == 0)
            {
                if (closing)
                    preformattedDepth = std::max(0, preformattedDepth - 1);
                else
                    ++preformattedDepth;
            }

            offset = end + 1;
        }

        return output.take();
    }
} // namespace javelin::jmap::render
