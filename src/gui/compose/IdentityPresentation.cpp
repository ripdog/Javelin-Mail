#include "gui/compose/IdentityPresentation.h"

#include <QStringList>
#include <QTextDocument>

namespace javelin::gui::compose
{
    namespace
    {
        [[nodiscard]] QString firstNonEmptyLine(QString text)
        {
            text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            for (const auto& line : text.split(QLatin1Char('\n')))
            {
                const auto simplified = line.simplified();
                if (!simplified.isEmpty())
                    return simplified;
            }
            return {};
        }
    } // namespace

    QString identityAddressLabel(const javelin::jmap::domain::Identity& identity)
    {
        if (identity.name.empty())
            return QString::fromStdString(identity.email);
        return QStringLiteral("%1 <%2>").arg(QString::fromStdString(identity.name),
                                             QString::fromStdString(identity.email));
    }

    QString identitySignaturePreview(const javelin::jmap::domain::Identity& identity)
    {
        if (identity.textSignature.has_value())
        {
            const auto preview = firstNonEmptyLine(QString::fromStdString(*identity.textSignature));
            if (!preview.isEmpty())
                return preview;
        }
        if (identity.htmlSignature.has_value())
        {
            QTextDocument document;
            document.setHtml(QString::fromStdString(*identity.htmlSignature));
            return firstNonEmptyLine(document.toPlainText());
        }
        return {};
    }

    QString composeIdentityDisplayText(const javelin::jmap::domain::Identity& identity,
                                       const QString& accountDisplayName,
                                       const std::size_t sameAddressIdentityCount,
                                       const bool includeAccountName)
    {
        QString text = identityAddressLabel(identity);
        if (sameAddressIdentityCount > 1)
        {
            const auto preview = identitySignaturePreview(identity);
            text += preview.isEmpty() ? QStringLiteral(" — No signature")
                                      : QStringLiteral(" — %1").arg(preview);
        }
        if (includeAccountName && !accountDisplayName.isEmpty())
            text += QStringLiteral(" — %1").arg(accountDisplayName);
        return text;
    }

    bool isWildcardSenderIdentity(const javelin::jmap::domain::Identity& identity)
    {
        return identity.email.starts_with("*@");
    }
} // namespace javelin::gui::compose
