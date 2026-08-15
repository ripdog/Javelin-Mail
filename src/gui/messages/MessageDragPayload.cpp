#include "gui/messages/MessageDragPayload.h"

#include <QDataStream>
#include <QString>

#include <cstdint>
#include <limits>

namespace javelin::gui::messages
{
    namespace
    {
        constexpr quint16 payloadVersion = 1;
        constexpr quint8 emailSelectionKind = 1;
        constexpr quint8 collapsedThreadSelectionKind = 2;
        constexpr quint32 maxSelectionItems = 100000;
    } // namespace

    QByteArray encodeMessageDragPayload(const MessageDragPayload& payload)
    {
        if (payload.sourceAccountId.empty() || payload.selection.empty() ||
            payload.selection.size() > maxSelectionItems)
            return {};

        QByteArray encoded;
        QDataStream stream{&encoded, QIODeviceBase::WriteOnly};
        stream.setVersion(QDataStream::Qt_6_0);
        stream << payloadVersion << QString::fromStdString(payload.sourceAccountId)
               << payload.sourceMailboxId.has_value();
        if (payload.sourceMailboxId.has_value())
            stream << QString::fromStdString(*payload.sourceMailboxId);
        stream << static_cast<quint32>(payload.selection.size());
        for (const auto& item : payload.selection)
        {
            if (const auto* email = std::get_if<javelin::app::SelectedEmail>(&item))
            {
                if (email->emailId.empty())
                    return {};
                stream << emailSelectionKind << QString::fromStdString(email->emailId);
                continue;
            }
            const auto* thread = std::get_if<javelin::app::SelectedCollapsedThread>(&item);
            if (thread == nullptr || thread->threadId.empty())
                return {};
            stream << collapsedThreadSelectionKind << QString::fromStdString(thread->threadId);
        }
        return stream.status() == QDataStream::Ok ? encoded : QByteArray{};
    }

    std::optional<MessageDragPayload> decodeMessageDragPayload(const QByteArray& payload)
    {
        if (payload.isEmpty())
            return std::nullopt;
        auto data = payload;
        QDataStream stream{&data, QIODeviceBase::ReadOnly};
        stream.setVersion(QDataStream::Qt_6_0);

        quint16 version = 0;
        QString sourceAccountId;
        bool hasSourceMailboxId = false;
        stream >> version >> sourceAccountId >> hasSourceMailboxId;
        if (stream.status() != QDataStream::Ok || version != payloadVersion ||
            sourceAccountId.isEmpty())
            return std::nullopt;

        std::optional<std::string> sourceMailboxId;
        if (hasSourceMailboxId)
        {
            QString mailboxId;
            stream >> mailboxId;
            if (stream.status() != QDataStream::Ok || mailboxId.isEmpty())
                return std::nullopt;
            sourceMailboxId = mailboxId.toStdString();
        }

        quint32 itemCount = 0;
        stream >> itemCount;
        if (stream.status() != QDataStream::Ok || itemCount == 0 || itemCount > maxSelectionItems)
            return std::nullopt;

        javelin::app::MessageSelection selection;
        selection.reserve(itemCount);
        for (quint32 index = 0; index < itemCount; ++index)
        {
            quint8 kind = 0;
            QString id;
            stream >> kind >> id;
            if (stream.status() != QDataStream::Ok || id.isEmpty())
                return std::nullopt;
            if (kind == emailSelectionKind)
                selection.emplace_back(javelin::app::SelectedEmail{.emailId = id.toStdString()});
            else if (kind == collapsedThreadSelectionKind)
                selection.emplace_back(
                    javelin::app::SelectedCollapsedThread{.threadId = id.toStdString()});
            else
                return std::nullopt;
        }
        if (!stream.atEnd())
            return std::nullopt;
        return MessageDragPayload{
            .sourceAccountId = sourceAccountId.toStdString(),
            .sourceMailboxId = std::move(sourceMailboxId),
            .selection = std::move(selection),
        };
    }

} // namespace javelin::gui::messages
