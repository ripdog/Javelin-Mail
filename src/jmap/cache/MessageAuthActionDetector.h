#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace javelin::jmap::cache
{

    enum class MessageAuthActionKind
    {
        CopyCode,
        OpenLink,
    };

    struct MessageAuthAction
    {
        MessageAuthActionKind kind = MessageAuthActionKind::CopyCode;
        QString value;
        int score = 0;

        friend bool operator==(const MessageAuthAction&, const MessageAuthAction&) = default;
    };

    struct MessageAuthActionDetection
    {
        std::optional<MessageAuthAction> action;
        QStringList diagnostics;

        [[nodiscard]] QString debugSummary() const;
    };

    struct MessageAuthActionInput
    {
        QString subject;
        QString plainText;
        QString html;
    };

    class MessageAuthActionDetector final
    {
      public:
        [[nodiscard]] MessageAuthActionDetection detect(const MessageAuthActionInput& input) const;
    };

} // namespace javelin::jmap::cache
