#pragma once

#include "jmap/domain/MailEntities.h"

#include <QString>

#include <functional>
#include <vector>

class QLabel;
class QVBoxLayout;
class QWidget;

namespace javelin::jmap::submission
{
    struct DraftSnapshot;
}

namespace javelin::gui::compose
{
    class ComposeRecipientController final
    {
      public:
        enum class RecipientType
        {
            To,
            Cc,
            Bcc,
        };

        ComposeRecipientController(QVBoxLayout& layout, QWidget& owner,
                                   std::function<void()> changed);
        ~ComposeRecipientController();

        void setSyncing(bool syncing);
        void setEnabled(bool enabled);
        void reset(const javelin::jmap::submission::DraftSnapshot& snapshot);
        void setText(RecipientType type, const QString& text);
        [[nodiscard]] QString text(RecipientType type) const;
        [[nodiscard]] std::vector<javelin::jmap::domain::EmailAddress>
        addresses(RecipientType type) const;
        void updateLabelWidths(QLabel& fromLabel, QLabel& subjectLabel);

      private:
        struct RecipientRow;

        void addRow(RecipientType type, const QString& text = {});
        void ensureTrailingRow();

        QVBoxLayout& m_layout;
        QWidget& m_owner;
        std::function<void()> m_changed;
        std::vector<RecipientRow> m_rows;
        bool m_syncing = false;
        int m_headerLabelWidth = 0;
    };
} // namespace javelin::gui::compose
