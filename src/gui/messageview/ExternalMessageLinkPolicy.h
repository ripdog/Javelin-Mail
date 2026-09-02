#pragma once

#include <QUrl>

namespace javelin::gui::messageview
{
    enum class MessageLinkAction
    {
        Reject,
        OpenExternal,
        ComposeMail,
    };

    [[nodiscard]] inline MessageLinkAction messageLinkAction(const QUrl& url)
    {
        if (!url.isValid())
            return MessageLinkAction::Reject;

        const auto scheme = url.scheme();
        if (scheme.compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 ||
            scheme.compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0)
        {
            return MessageLinkAction::OpenExternal;
        }
        if (scheme.compare(QStringLiteral("mailto"), Qt::CaseInsensitive) == 0)
            return MessageLinkAction::ComposeMail;
        return MessageLinkAction::Reject;
    }
} // namespace javelin::gui::messageview
