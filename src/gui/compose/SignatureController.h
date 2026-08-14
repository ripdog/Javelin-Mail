#pragma once

#include <QTextCursor>

#include <functional>

class QComboBox;

namespace javelin::jmap::submission
{
    struct DraftSnapshot;
}

namespace javelin::gui::compose
{
    class JavelinComposerEdit;

    class SignatureController final
    {
      public:
        SignatureController(QComboBox& identities, JavelinComposerEdit& editor,
                            javelin::jmap::submission::DraftSnapshot& snapshot,
                            std::function<void()> changed);

        void initialize();
        void replaceForIdentity(int index, bool forceInsert = false);
        void remove();
        void restoreIdentity(int index);
        void noteDocumentChange(int position, int removed, int added, bool uiSyncing);

        [[nodiscard]] bool shouldRestoreAfterIdentityReload() const;
        [[nodiscard]] bool detachForBodyFormatSwitch();

      private:
        [[nodiscard]] QString plainTextForIndex(int index) const;
        [[nodiscard]] QString htmlForIndex(int index) const;
        [[nodiscard]] int defaultInsertionPosition() const;

        QComboBox& m_identities;
        JavelinComposerEdit& m_editor;
        javelin::jmap::submission::DraftSnapshot& m_snapshot;
        std::function<void()> m_changed;
        bool m_programmaticEdit = false;
        bool m_tracked = false;
        bool m_custom = false;
        bool m_explicitlyRemoved = false;
        int m_insertionPosition = -1;
        QTextCursor m_cursor;
    };
} // namespace javelin::gui::compose
