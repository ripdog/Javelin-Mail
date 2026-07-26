#pragma once

namespace javelin::app
{
    class ComposePreferences
    {
      public:
        [[nodiscard]] static int undoSendDelaySeconds();
        static void setUndoSendDelaySeconds(int seconds);
    };
} // namespace javelin::app
