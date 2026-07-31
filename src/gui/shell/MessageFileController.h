#pragma once

#include "jmap/OperationError.h"

#include <QObject>
#include <QPointer>
#include <QTemporaryDir>

#include <string>

class QWidget;

namespace javelin::app
{
    class MailApplicationService;
}

namespace javelin::jmap::cache
{
    class MessageViewReader;
}

namespace javelin::gui::shell
{

    class MessageFileController final : public QObject
    {
        Q_OBJECT

      public:
        MessageFileController(javelin::app::MailApplicationService& mailService,
                              javelin::jmap::cache::MessageViewReader& messageViewReader,
                              QWidget* dialogParent, QObject* parent = nullptr);

        void saveAttachment(std::string accountId, std::string emailId, std::string partId);
        void saveAllAttachments(std::string accountId, std::string emailId);
        void openAttachment(std::string accountId, std::string emailId, std::string partId);
        void viewMessageSource(std::string accountId, std::string emailId);

      Q_SIGNALS:
        void statusMessage(QString message, int durationMilliseconds);
        void operationFailed(javelin::jmap::OperationError error);
        void userInterventionRequired(QString message);

      private:
        javelin::app::MailApplicationService& m_mailService;
        javelin::jmap::cache::MessageViewReader& m_messageViewReader;
        QPointer<QWidget> m_dialogParent;
        QTemporaryDir m_temporaryDirectory;
    };

} // namespace javelin::gui::shell
