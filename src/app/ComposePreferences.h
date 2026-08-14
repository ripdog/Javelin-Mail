#pragma once

namespace javelin::app
{
    class ComposePreferences
    {
      public:
        [[nodiscard]] static int undoSendDelaySeconds();
        [[nodiscard]] static bool undoSendUsesDialog();
        static void setUndoSendDelaySeconds(int seconds);
    };
} // namespace javelin::app
