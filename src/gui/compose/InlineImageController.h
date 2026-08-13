#pragma once

#include <QObject>
#include <QString>

#include <cstddef>
#include <functional>
#include <string>

class QImage;

namespace javelin::jmap::submission
{
    struct DraftSnapshot;
}

namespace javelin::gui::compose
{
    class JavelinComposerEdit;

    class InlineImageController final : public QObject
    {
        Q_OBJECT

      public:
        InlineImageController(JavelinComposerEdit& editor,
                              javelin::jmap::submission::DraftSnapshot& snapshot,
                              std::function<void()> attachmentsChanged,
                              std::function<void(QString, int)> statusMessage,
                              std::function<void()> pendingStateChanged,
                              std::function<void(bool)> allProcessingFinished,
                              QObject* parent = nullptr);

        [[nodiscard]] bool hasPendingJobs() const;

        void addImagePath(const QString& filePath);
        void addPastedImage(const QImage& image);
        void adoptInsertedComposerImage(int insertionPosition, const QString& sourceFilePath);
        [[nodiscard]] bool setAttachmentEmbedded(std::size_t index, bool embedded);
        void insertEmbeddedImage(std::size_t index);
        void removeEmbeddedImageReference(const std::string& contentId);
        void setEditorHtml(const QString& html);
        void loadResources();
        [[nodiscard]] QString stableHtml() const;
        void reconcileAttachmentReferences(const QString& html);

      private:
        void finishPreparation();

        JavelinComposerEdit& m_editor;
        javelin::jmap::submission::DraftSnapshot& m_snapshot;
        std::function<void()> m_attachmentsChanged;
        std::function<void(QString, int)> m_statusMessage;
        std::function<void()> m_pendingStateChanged;
        std::function<void(bool)> m_allProcessingFinished;
        std::size_t m_pendingJobs = 0;
        bool m_processingSucceeded = true;
    };
} // namespace javelin::gui::compose
