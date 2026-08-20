#pragma once

#include "app/MessageContentApplicationPorts.h"
#include "jmap/OperationError.h"

#include <QObject>
#include <QPointer>

#include <string>

class KUiServerV2JobTracker;
class QWidget;

namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::app
{
    class MessageContentPort;
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
        MessageFileController(javelin::gui::settings::GuiSettings& settings,
                              javelin::app::MessageContentPort& contentPort,
                              javelin::jmap::cache::MessageViewReader& messageViewReader,
                              QWidget* dialogParent, QObject* parent = nullptr);

        void saveAttachment(std::string accountId, std::string emailId, std::string partId);
        void saveAllAttachments(std::string accountId, std::string emailId);
        void openAttachment(std::string accountId, std::string emailId, std::string partId);
        void openAttachmentWith(std::string accountId, std::string emailId, std::string partId);
        void saveMessages(std::string accountId, std::optional<std::string> sourceMailboxId,
                          javelin::app::MessageSelection selection);
        void viewMessageSource(std::string accountId, std::string emailId);

      Q_SIGNALS:
        void statusMessage(QString message, int durationMilliseconds);
        void operationFailed(javelin::jmap::OperationError error);
        void userInterventionRequired(QString message);

      private:
        void openAttachment(std::string accountId, std::string emailId, std::string partId,
                            bool chooseApplication);
        void launchTemporaryFile(QString path, bool chooseApplication, QString successMessage);
        javelin::gui::settings::GuiSettings& m_settings;
        javelin::app::MessageContentPort& m_contentPort;
        javelin::jmap::cache::MessageViewReader& m_messageViewReader;
        QPointer<QWidget> m_dialogParent;
        KUiServerV2JobTracker* m_fileJobTracker = nullptr;
    };

} // namespace javelin::gui::shell
