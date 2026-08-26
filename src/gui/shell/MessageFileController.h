#pragma once

#include "app/MessageContentApplicationPorts.h"
#include "gui/messages/MessageDragPayload.h"
#include "jmap/OperationError.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QUrl>

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
        void dragAttachment(QString accountId, QString emailId, QString partId, QWidget* source);
        void prepareMessageDrag(quint64 requestId,
                                javelin::gui::messages::MessageDragPayload payload);
        void saveMessages(std::string accountId, std::optional<std::string> sourceMailboxId,
                          javelin::app::MessageSelection selection);
        void viewMessageSource(std::string accountId, std::string emailId);

      Q_SIGNALS:
        void statusMessage(QString message, int durationMilliseconds);
        void operationFailed(javelin::jmap::OperationError error);
        void userInterventionRequired(QString message);
        void messageDragReady(quint64 requestId, QList<QUrl> urls);
        void messageDragFailed(quint64 requestId);

      private:
        void openAttachment(std::string accountId, std::string emailId, std::string partId,
                            bool chooseApplication);
        void launchTemporaryFile(QString path, bool chooseApplication, QString successMessage);
        void startExternalFileDrag(QWidget* source, const QList<QUrl>& urls);
        [[nodiscard]] QString attachmentDragKey(const QString& accountId, const QString& emailId,
                                                const QString& partId) const;
        javelin::gui::settings::GuiSettings& m_settings;
        javelin::app::MessageContentPort& m_contentPort;
        javelin::jmap::cache::MessageViewReader& m_messageViewReader;
        QPointer<QWidget> m_dialogParent;
        KUiServerV2JobTracker* m_fileJobTracker = nullptr;
        QString m_externalDragRootPath;
        QHash<QString, QUrl> m_preparedAttachmentDragFiles;
    };

} // namespace javelin::gui::shell
