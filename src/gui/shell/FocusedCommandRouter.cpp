#include "gui/shell/FocusedCommandRouter.h"

#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextDocument>
#include <QTextEdit>
#include <QWidget>

namespace javelin::gui::shell
{

    QWidget* FocusedCommandRouter::editorFor(QWidget* focus)
    {
        if (auto* comboBox = qobject_cast<QComboBox*>(focus);
            comboBox != nullptr && comboBox->isEditable())
        {
            return comboBox->lineEdit();
        }
        return focus;
    }

    bool FocusedCommandRouter::isNativeCommandAvailable(QWidget* focus,
                                                        const EditHistoryDirection direction)
    {
        auto* editor = editorFor(focus);
        if (const auto* lineEdit = qobject_cast<QLineEdit*>(editor))
        {
            return direction == EditHistoryDirection::Undo ? lineEdit->isUndoAvailable()
                                                           : lineEdit->isRedoAvailable();
        }
        if (const auto* textEdit = qobject_cast<QTextEdit*>(editor))
        {
            return direction == EditHistoryDirection::Undo
                       ? textEdit->document()->isUndoAvailable()
                       : textEdit->document()->isRedoAvailable();
        }
        if (const auto* plainTextEdit = qobject_cast<QPlainTextEdit*>(editor))
        {
            return direction == EditHistoryDirection::Undo
                       ? plainTextEdit->document()->isUndoAvailable()
                       : plainTextEdit->document()->isRedoAvailable();
        }
        return false;
    }

    bool FocusedCommandRouter::invokeNativeCommand(QWidget* focus,
                                                   const EditHistoryDirection direction)
    {
        auto* editor = editorFor(focus);
        if (!isNativeCommandAvailable(editor, direction))
            return false;

        if (auto* lineEdit = qobject_cast<QLineEdit*>(editor))
        {
            direction == EditHistoryDirection::Undo ? lineEdit->undo() : lineEdit->redo();
            return true;
        }
        if (auto* textEdit = qobject_cast<QTextEdit*>(editor))
        {
            direction == EditHistoryDirection::Undo ? textEdit->undo() : textEdit->redo();
            return true;
        }
        if (auto* plainTextEdit = qobject_cast<QPlainTextEdit*>(editor))
        {
            direction == EditHistoryDirection::Undo ? plainTextEdit->undo() : plainTextEdit->redo();
            return true;
        }
        return false;
    }

} // namespace javelin::gui::shell
