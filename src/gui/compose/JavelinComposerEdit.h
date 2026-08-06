#pragma once

#include <MessageComposer/RichTextComposerNg>

#include <QImage>
#include <QString>
#include <QStringList>

class QMimeData;

namespace MessageComposer
{
    class TextPart;
}

namespace javelin::gui::compose
{

    [[nodiscard]] QString sanitizeComposerPasteHtml(QString html);

    class JavelinComposerEdit final : public MessageComposer::RichTextComposerNg
    {
        Q_OBJECT

      public:
        explicit JavelinComposerEdit(QWidget* parent = nullptr);

        void fillComposerTextPart(MessageComposer::TextPart* textPart);
        [[nodiscard]] QString toCleanHtml() const;
        [[nodiscard]] QString toCleanPlainText() const;

      Q_SIGNALS:
        void attachmentPathsRequested(const QStringList& filePaths);
        void inlineImageRequested(const QImage& image);

      protected:
        void insertFromMimeData(const QMimeData* source) override;
        [[nodiscard]] bool canInsertFromMimeData(const QMimeData* source) const override;
    };

} // namespace javelin::gui::compose
