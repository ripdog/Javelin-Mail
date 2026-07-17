#include "gui/widgets/EmailAddressLineEdit.h"

#include "app/AddressSuggestionStore.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QTextBlock>
#include <QTextCursor>

#include <algorithm>

namespace javelin::gui::widgets
{
    EmailAddressLineEdit::EmailAddressLineEdit(const bool multipleAddresses, QWidget* parent)
        : QLineEdit(parent)
    {
        configureCompletion(multipleAddresses);
    }

    EmailAddressLineEdit::EmailAddressLineEdit(const QString& text, const bool multipleAddresses,
                                               QWidget* parent)
        : QLineEdit(text, parent)
    {
        configureCompletion(multipleAddresses);
    }

    void EmailAddressLineEdit::configureCompletion(const bool multipleAddresses)
    {
        m_completer =
            new QCompleter(&javelin::app::AddressSuggestionStore::instance().model(), this);
        m_completer->setWidget(this);
        m_completer->setCaseSensitivity(Qt::CaseInsensitive);
        m_completer->setCompletionMode(QCompleter::PopupCompletion);
        m_completer->setFilterMode(Qt::MatchContains);
        if (!multipleAddresses)
        {
            setCompleter(m_completer);
            return;
        }
        connect(this, &QLineEdit::textEdited, m_completer,
                [this](const QString& value)
                {
                    const qsizetype separator = std::max(value.lastIndexOf(QLatin1Char(',')),
                                                         value.lastIndexOf(QLatin1Char(';')));
                    const QString token = value.sliced(separator + 1).trimmed();
                    if (token.isEmpty())
                    {
                        m_completer->popup()->hide();
                        return;
                    }
                    m_completer->setCompletionPrefix(token);
                    m_completer->complete();
                });
        connect(m_completer, qOverload<const QString&>(&QCompleter::activated), this,
                [this](const QString& completion)
                {
                    const qsizetype separator = std::max(text().lastIndexOf(QLatin1Char(',')),
                                                         text().lastIndexOf(QLatin1Char(';')));
                    const QString prefix = separator >= 0
                                               ? text().left(separator + 1) + QStringLiteral(" ")
                                               : QString{};
                    setText(prefix + completion);
                    setCursorPosition(static_cast<int>(text().size()));
                });
    }

    EmailAddressPlainTextEdit::EmailAddressPlainTextEdit(QWidget* parent) : QPlainTextEdit(parent)
    {
        m_completer =
            new QCompleter(&javelin::app::AddressSuggestionStore::instance().model(), this);
        m_completer->setWidget(this);
        m_completer->setCaseSensitivity(Qt::CaseInsensitive);
        m_completer->setCompletionMode(QCompleter::PopupCompletion);
        m_completer->setFilterMode(Qt::MatchContains);
        connect(this, &QPlainTextEdit::textChanged, this,
                [this]
                {
                    const QString prefix = textCursor().block().text().trimmed();
                    if (prefix.isEmpty())
                    {
                        m_completer->popup()->hide();
                        return;
                    }
                    m_completer->setCompletionPrefix(prefix);
                    m_completer->complete(cursorRect());
                });
        connect(m_completer, qOverload<const QString&>(&QCompleter::activated), this,
                [this](const QString& completion)
                {
                    QTextCursor cursor = textCursor();
                    cursor.movePosition(QTextCursor::StartOfBlock);
                    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                    cursor.insertText(completion);
                    setTextCursor(cursor);
                });
    }
} // namespace javelin::gui::widgets
