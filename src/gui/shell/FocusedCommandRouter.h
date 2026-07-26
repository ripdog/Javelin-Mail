#pragma once

class QWidget;

namespace javelin::gui::shell
{

    enum class EditHistoryDirection
    {
        Undo,
        Redo,
    };

    class FocusedCommandRouter
    {
      public:
        [[nodiscard]] static bool isNativeCommandAvailable(QWidget* focus,
                                                           EditHistoryDirection direction);
        [[nodiscard]] static bool invokeNativeCommand(QWidget* focus,
                                                      EditHistoryDirection direction);

      private:
        [[nodiscard]] static QWidget* editorFor(QWidget* focus);
    };

} // namespace javelin::gui::shell
