#pragma once

namespace javelin::app
{
    class ComposePreferences
    {
      public:
        [[nodiscard]] static int undoSendDelaySeconds();
        [[nodiscard]] static bool undoSendUsesDialog();
    };
} // namespace javelin::app
