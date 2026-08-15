#include "gui/messages/MessageConfirmationPresentation.h"

#include "gui/messages/MessageListModel.h"

#include <KLocalizedString>

#include <QDateTime>
#include <QLocale>
#include <QStringList>

#include <algorithm>
#include <ranges>

namespace javelin::gui::messages
{
    namespace
    {
        constexpr qsizetype maxDetailedItems = 6;

        [[nodiscard]] QString formattedTimestamp(const QModelIndex& index)
        {
            const auto raw = index.data(MessageListModel::ReceivedAtRole).toString();
            const auto dateTime = QDateTime::fromString(raw, Qt::ISODate);
            return dateTime.isValid()
                       ? QLocale{}.toString(dateTime.toLocalTime(), QLocale::ShortFormat)
                       : raw;
        }

        [[nodiscard]] QString senderLabel(const QModelIndex& index)
        {
            auto sender = index.data(MessageListModel::SenderDisplayRole).toString().trimmed();
            if (sender.isEmpty())
                sender = index.data(MessageListModel::SenderEmailRole).toString().trimmed();
            return sender.isEmpty() ? i18n("Unknown sender") : sender;
        }

        [[nodiscard]] QString itemDetails(const QModelIndex& index)
        {
            auto subject = index.data(MessageListModel::SubjectRole).toString().trimmed();
            if (subject.isEmpty())
                subject = i18n("(no subject)");

            QStringList parts{QStringLiteral("“%1”").arg(subject), senderLabel(index)};
            const auto timestamp = formattedTimestamp(index);
            if (!timestamp.isEmpty())
                parts.push_back(timestamp);

            const auto kind = static_cast<MessageListModel::RowKind>(
                index.data(MessageListModel::RowKindRole).toInt());
            const bool collapsedThread = kind == MessageListModel::RowKind::ThreadSummary &&
                                         !index.data(MessageListModel::IsExpandedRole).toBool();
            if (collapsedThread)
            {
                auto count = index.data(MessageListModel::GlobalThreadMessageCountRole);
                if (!count.isValid())
                    count = index.data(MessageListModel::ThreadMessageCountRole);
                if (count.isValid() && count.toULongLong() > 0)
                    parts.push_back(i18np("%1 message", "%1 messages", count.toULongLong()));
            }
            return parts.join(QStringLiteral(" — "));
        }
    } // namespace

    PermanentDeleteConfirmation
    permanentDeleteConfirmation(const javelin::app::MessageSelection& selection,
                                QModelIndexList selectedRows, const QModelIndex& currentIndex)
    {
        const auto emailCount = static_cast<std::size_t>(std::ranges::count_if(
            selection, [](const auto& item)
            { return std::holds_alternative<javelin::app::SelectedEmail>(item); }));
        const auto threadCount = selection.size() - emailCount;

        QString prompt;
        if (selection.size() == 1 && threadCount == 1)
            prompt = i18n("Permanently delete every message in the selected conversation? This "
                          "cannot be undone.");
        else if (selection.size() == 1)
            prompt = i18n("Permanently delete the selected message? This cannot be undone.");
        else if (threadCount == 0)
            prompt = i18np("Permanently delete %1 selected message? This cannot be undone.",
                           "Permanently delete %1 selected messages? This cannot be undone.",
                           selection.size());
        else if (emailCount == 0)
            prompt = i18np("Permanently delete %1 selected conversation? This cannot be undone.",
                           "Permanently delete %1 selected conversations? This cannot be undone.",
                           selection.size());
        else
            prompt = i18n("Permanently delete the selected messages and conversations? This cannot "
                          "be undone.");

        selectedRows.erase(std::remove_if(selectedRows.begin(), selectedRows.end(),
                                          [](const QModelIndex& index)
                                          { return !index.isValid(); }),
                           selectedRows.end());
        if (selectedRows.isEmpty() && currentIndex.isValid())
            selectedRows.push_back(currentIndex);
        std::ranges::sort(selectedRows, [](const QModelIndex& left, const QModelIndex& right)
                          { return left.row() < right.row(); });

        QStringList details;
        const qsizetype visibleCount = std::min(maxDetailedItems, selectedRows.size());
        details.reserve(visibleCount + 1);
        for (qsizetype index = 0; index < visibleCount; ++index)
            details.push_back(itemDetails(selectedRows.at(index)));
        if (selectedRows.size() > visibleCount)
            details.push_back(i18np("…and %1 more selected item", "…and %1 more selected items",
                                    selectedRows.size() - visibleCount));

        return {.prompt = std::move(prompt), .details = details.join(QLatin1Char('\n'))};
    }
} // namespace javelin::gui::messages
