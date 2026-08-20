#pragma once

#include "jmap/cache/MessageViewReader.h"

#include <QWidget>

#include <optional>
#include <string>
#include <vector>

class QGridLayout;
class QLabel;
class QResizeEvent;
class QToolButton;

namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::gui::messageview
{
    class MessageAttachmentPanel final : public QWidget
    {
        Q_OBJECT

      public:
        MessageAttachmentPanel(
            javelin::gui::settings::GuiSettings& settings,
            const std::optional<std::string>& accountId, const std::optional<std::string>& emailId,
            const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot,
            QWidget* parent = nullptr);

        void refresh();
        [[nodiscard]] bool hasVisibleAttachments() const;
        [[nodiscard]] QString statusText() const;

      Q_SIGNALS:
        void saveAttachmentRequested(QString accountId, QString emailId, QString partId);
        void openAttachmentRequested(QString accountId, QString emailId, QString partId);
        void openAttachmentWithRequested(QString accountId, QString emailId, QString partId);
        void saveAllAttachmentsRequested(QString accountId, QString emailId);

      protected:
        void resizeEvent(QResizeEvent* event) override;

      private:
        void rebuildRows();
        void reflowRows();

        javelin::gui::settings::GuiSettings& m_settings;
        const std::optional<std::string>& m_accountId;
        const std::optional<std::string>& m_emailId;
        const std::optional<javelin::jmap::cache::MessageViewSnapshot>& m_snapshot;
        QLabel* m_statusLabel = nullptr;
        QToolButton* m_saveAllButton = nullptr;
        QWidget* m_listWidget = nullptr;
        QGridLayout* m_listLayout = nullptr;
        std::vector<QWidget*> m_attachmentTiles;
    };
} // namespace javelin::gui::messageview
