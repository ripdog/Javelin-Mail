#include "gui/compose/SignatureController.h"

#include "gui/compose/ComposeBodyConverter.h"
#include "gui/compose/ComposeIdentityController.h"
#include "gui/compose/JavelinComposerEdit.h"
#include "gui/compose/SignatureTrackingPolicy.h"
#include "jmap/submission/ComposeTypes.h"

#include <QComboBox>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>
#include <cmath>
#include <utility>

namespace javelin::gui::compose
{
    SignatureController::SignatureController(QComboBox& identities, JavelinComposerEdit& editor,
                                             javelin::jmap::submission::DraftSnapshot& snapshot,
                                             std::function<void()> changed)
        : m_identities(identities), m_editor(editor), m_snapshot(snapshot),
          m_changed(std::move(changed))
    {
    }

    QString SignatureController::plainTextForIndex(const int index) const
    {
        if (index < 0 || index >= m_identities.count())
            return {};
        const auto text =
            m_identities.itemData(index, ComposeIdentityController::textSignatureRole).toString();
        const auto html =
            m_identities.itemData(index, ComposeIdentityController::htmlSignatureRole).toString();
        if (m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RichText &&
            !html.isEmpty())
            return plainTextFromHtml(html);
        if (!text.isEmpty())
            return text;
        return html.isEmpty() ? QString{} : plainTextFromHtml(html);
    }

    QString SignatureController::htmlForIndex(const int index) const
    {
        if (index < 0 || index >= m_identities.count())
            return {};
        const auto html =
            m_identities.itemData(index, ComposeIdentityController::htmlSignatureRole).toString();
        if (!html.isEmpty())
            return html;
        const auto text =
            m_identities.itemData(index, ComposeIdentityController::textSignatureRole).toString();
        return text.isEmpty() ? QString{} : htmlFromPlainText(text);
    }

    int SignatureController::defaultInsertionPosition() const
    {
        if (m_insertionPosition >= 0 &&
            m_insertionPosition <= m_editor.document()->characterCount() - 1)
            return m_insertionPosition;

        for (auto block = m_editor.document()->begin(); block.isValid(); block = block.next())
        {
            const auto format = block.blockFormat();
            if (std::abs(format.leftMargin() - 40.0) < 0.01 &&
                std::abs(format.rightMargin() - 40.0) < 0.01)
                return block.position();
            if (block.text().contains(QStringLiteral("---------- Forwarded message ----------")))
                return block.position();
        }
        return std::max(0, m_editor.document()->characterCount() - 1);
    }

    void SignatureController::initialize()
    {
        m_tracked = false;
        m_custom = false;
        m_explicitlyRemoved = false;
        m_insertionPosition = -1;
        const auto signature = plainTextForIndex(m_identities.currentIndex());
        if (signature.trimmed().isEmpty())
        {
            m_insertionPosition = defaultInsertionPosition();
            return;
        }
        auto cursor = m_editor.document()->find(signature);
        if (cursor.isNull())
        {
            m_insertionPosition = defaultInsertionPosition();
            return;
        }
        m_cursor = cursor;
        m_insertionPosition = cursor.selectionStart();
        m_tracked = true;
    }

    void SignatureController::replaceForIdentity(const int index, const bool forceInsert)
    {
        if (!shouldReplaceTrackedSignature(m_tracked, m_custom, m_explicitlyRemoved, forceInsert))
            return;

        const auto plain = plainTextForIndex(index);
        const auto html = htmlForIndex(index);
        const bool wasTracked = m_tracked;
        const bool hadExplicitRemoval = m_explicitlyRemoved;
        const int start = wasTracked ? m_cursor.selectionStart() : defaultInsertionPosition();
        m_programmaticEdit = true;
        QTextCursor cursor{m_editor.document()};
        cursor.setPosition(start);
        if (wasTracked)
        {
            cursor.setPosition(m_cursor.selectionEnd(), QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
        }
        else if (forceInsert && !hadExplicitRemoval && (!plain.isEmpty() || !html.isEmpty()))
        {
            if (m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RichText)
                cursor.insertHtml(QStringLiteral("<p><br/></p>"));
            else
            {
                const auto body = m_editor.toPlainText();
                cursor.insertText(body.isEmpty() ? QStringLiteral("\n") : QStringLiteral("\n\n"));
            }
        }
        const int insertionStart = cursor.position();
        if (m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RichText)
        {
            if (!html.isEmpty())
                cursor.insertHtml(html);
        }
        else if (!plain.isEmpty())
            cursor.insertText(plain);
        const int insertionEnd = cursor.position();
        m_programmaticEdit = false;

        m_insertionPosition = insertionStart;
        m_tracked = insertionEnd > insertionStart;
        m_custom = false;
        m_explicitlyRemoved = false;
        if (m_tracked)
        {
            m_cursor = QTextCursor{m_editor.document()};
            m_cursor.setPosition(insertionStart);
            m_cursor.setPosition(insertionEnd, QTextCursor::KeepAnchor);
        }
        if (m_changed)
            m_changed();
    }

    void SignatureController::remove()
    {
        if (!m_tracked)
        {
            m_explicitlyRemoved = true;
            return;
        }
        m_insertionPosition = m_cursor.selectionStart();
        m_programmaticEdit = true;
        auto cursor = m_cursor;
        cursor.removeSelectedText();
        m_programmaticEdit = false;
        m_tracked = false;
        m_custom = false;
        m_explicitlyRemoved = true;
        if (m_changed)
            m_changed();
    }

    void SignatureController::restoreIdentity(const int index)
    {
        m_explicitlyRemoved = false;
        m_custom = false;
        replaceForIdentity(index, true);
    }

    void SignatureController::noteDocumentChange(const int position, const int removed,
                                                 const int added, const bool uiSyncing)
    {
        if (uiSyncing || m_programmaticEdit || !m_tracked)
            return;
        if (changeTouchesTrackedSignature(
                {.start = m_cursor.selectionStart(), .end = m_cursor.selectionEnd()}, position,
                removed, added))
            m_custom = true;
    }

    bool SignatureController::shouldRestoreAfterIdentityReload() const
    {
        return m_tracked && !m_custom && !m_explicitlyRemoved;
    }

    bool SignatureController::detachForBodyFormatSwitch()
    {
        const bool restore = shouldRestoreAfterIdentityReload();
        if (!restore)
            return false;
        m_insertionPosition = m_cursor.selectionStart();
        m_programmaticEdit = true;
        auto cursor = m_cursor;
        cursor.removeSelectedText();
        m_programmaticEdit = false;
        m_tracked = false;
        return true;
    }
} // namespace javelin::gui::compose
