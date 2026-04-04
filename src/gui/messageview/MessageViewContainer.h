#pragma once

#include <QWidget>

#include <optional>
#include <string>

class QLabel;

namespace javelin::gui::messageview
{

    class MessageViewContainer : public QWidget
    {
        Q_OBJECT

      public:
        explicit MessageViewContainer(QWidget* parent = nullptr);
        ~MessageViewContainer() override;

        void setSelection(std::optional<std::string> accountId,
                          std::optional<std::string> mailboxId, std::optional<std::string> emailId);

      private:
        void updatePresentation();

        std::optional<std::string> m_accountId;
        std::optional<std::string> m_mailboxId;
        std::optional<std::string> m_emailId;
        QLabel* m_titleLabel = nullptr;
        QLabel* m_detailLabel = nullptr;
        QLabel* m_bodyLabel = nullptr;
    };

} // namespace javelin::gui::messageview
