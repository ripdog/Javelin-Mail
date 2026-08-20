#include "gui/messageview/MessageReaderCommandController.h"

#include "gui/messageview/HtmlMessageView.h"
#include "gui/messageview/MessageBodyPresenter.h"

#include <KLocalizedString>

#include <QDialog>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPrintDialog>
#include <QPrinter>
#include <QShortcut>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolButton>

#include <cmath>
#include <utility>
#include <vector>

namespace javelin::gui::messageview
{
    MessageReaderCommandController::MessageReaderCommandController(
        MessageBodyPresenter& bodyPresenter, QTextBrowser& plainTextView, HtmlMessageView& htmlView,
        QWidget& findBarContainer, QLineEdit& findEdit, QLabel& findResultLabel,
        QToolButton& findPreviousButton, QToolButton& findNextButton,
        std::function<bool()> contentAvailable, std::function<void()> focusMessageBody,
        std::function<QString()> documentName, QWidget& dialogParent, QObject* parent)
        : QObject(parent), m_bodyPresenter(bodyPresenter), m_plainTextView(plainTextView),
          m_htmlView(htmlView), m_findBarContainer(findBarContainer), m_findEdit(findEdit),
          m_findResultLabel(findResultLabel), m_findPreviousButton(findPreviousButton),
          m_findNextButton(findNextButton), m_contentAvailable(std::move(contentAvailable)),
          m_focusMessageBody(std::move(focusMessageBody)), m_documentName(std::move(documentName)),
          m_dialogParent(dialogParent)
    {
        m_findEdit.installEventFilter(this);
        connect(&m_findEdit, &QLineEdit::textChanged, this,
                [this]
                {
                    m_plainTextFindQuery.clear();
                    m_plainTextFindIndex = -1;
                    runFind(false);
                });
        connect(&m_findEdit, &QLineEdit::returnPressed, this,
                [this]
                {
                    if (QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
                        findPrevious();
                    else
                        findNext();
                });
        connect(&m_findPreviousButton, &QToolButton::clicked, this,
                &MessageReaderCommandController::findPrevious);
        connect(&m_findNextButton, &QToolButton::clicked, this,
                &MessageReaderCommandController::findNext);

        auto* dismissFindShortcut =
            new QShortcut(QKeySequence{Qt::Key_Escape}, &m_findBarContainer);
        dismissFindShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(dismissFindShortcut, &QShortcut::activated, this,
                &MessageReaderCommandController::dismissFindBar);
    }

    bool MessageReaderCommandController::available() const
    {
        if (!m_contentAvailable())
            return false;
        const auto view = m_bodyPresenter.activeView();
        return view == MessageBodyPresenter::View::Html ||
               view == MessageBodyPresenter::View::PlainText;
    }

    void MessageReaderCommandController::showFindBar()
    {
        if (!available())
            return;
        m_findBarContainer.setVisible(true);
        m_findEdit.setFocus(Qt::ShortcutFocusReason);
        m_findEdit.selectAll();
        if (!m_findEdit.text().isEmpty())
            runFind(false);
    }

    void MessageReaderCommandController::dismissFindBar()
    {
        clearFindHighlights();
        m_findBarContainer.setVisible(false);
        m_focusMessageBody();
    }

    void MessageReaderCommandController::findNext()
    {
        runFind(false);
    }

    void MessageReaderCommandController::findPrevious()
    {
        runFind(true);
    }

    void MessageReaderCommandController::runFind(const bool backwards)
    {
        if (!available())
        {
            updateFindResult(0, 0);
            return;
        }

        const QString query = m_findEdit.text();
        if (query.isEmpty())
        {
            clearFindHighlights();
            return;
        }

        if (m_bodyPresenter.activeView() == MessageBodyPresenter::View::Html)
        {
            m_htmlView.findText(query, backwards,
                                [this, query](const int activeMatch, const int matchCount)
                                {
                                    if (m_findEdit.text() == query &&
                                        m_bodyPresenter.activeView() ==
                                            MessageBodyPresenter::View::Html)
                                    {
                                        updateFindResult(activeMatch, matchCount);
                                    }
                                });
            return;
        }

        const bool newQuery = m_plainTextFindQuery != query;
        std::vector<QTextCursor> matches;
        QTextCursor cursor{m_plainTextView.document()};
        while (true)
        {
            cursor = m_plainTextView.document()->find(query, cursor);
            if (cursor.isNull())
                break;
            matches.push_back(cursor);
        }

        if (matches.empty())
        {
            m_plainTextFindQuery = query;
            m_plainTextFindIndex = -1;
            m_plainTextView.setExtraSelections({});
            updateFindResult(0, 0);
            return;
        }

        const int matchCount = static_cast<int>(matches.size());
        if (newQuery || m_plainTextFindIndex < 0 || m_plainTextFindIndex >= matchCount)
        {
            m_plainTextFindIndex = backwards ? matchCount - 1 : 0;
        }
        else if (backwards)
        {
            m_plainTextFindIndex = (m_plainTextFindIndex + matchCount - 1) % matchCount;
        }
        else
        {
            m_plainTextFindIndex = (m_plainTextFindIndex + 1) % matchCount;
        }
        m_plainTextFindQuery = query;

        QList<QTextEdit::ExtraSelection> highlights;
        highlights.reserve(matchCount);
        for (const auto& match : matches)
        {
            QTextEdit::ExtraSelection selection;
            selection.cursor = match;
            selection.format.setBackground(
                m_findBarContainer.palette().brush(QPalette::AlternateBase));
            highlights.push_back(std::move(selection));
        }
        m_plainTextView.setExtraSelections(highlights);
        m_plainTextView.setTextCursor(matches[static_cast<std::size_t>(m_plainTextFindIndex)]);
        m_plainTextView.ensureCursorVisible();
        updateFindResult(m_plainTextFindIndex + 1, matchCount);
    }

    void MessageReaderCommandController::clearFindHighlights()
    {
        m_htmlView.clearFindHighlights();
        m_plainTextView.setExtraSelections({});
        auto cursor = m_plainTextView.textCursor();
        cursor.clearSelection();
        m_plainTextView.setTextCursor(cursor);
        m_plainTextFindQuery.clear();
        m_plainTextFindIndex = -1;
        updateFindResult(0, 0);
    }

    void MessageReaderCommandController::resetFind(const bool hideBar)
    {
        clearFindHighlights();
        if (hideBar)
            m_findBarContainer.setVisible(false);
    }

    void MessageReaderCommandController::updateFindResult(const int activeMatch,
                                                          const int matchCount)
    {
        const bool hasQuery = !m_findEdit.text().isEmpty();
        if (!hasQuery)
        {
            m_findResultLabel.clear();
        }
        else if (matchCount <= 0)
        {
            m_findResultLabel.setText(i18nc("@info find result", "No matches"));
        }
        else
        {
            m_findResultLabel.setText(
                i18nc("@info find result count", "%1 of %2", activeMatch, matchCount));
        }
        const bool canNavigate = hasQuery && matchCount > 0;
        m_findPreviousButton.setEnabled(canNavigate);
        m_findNextButton.setEnabled(canNavigate);
    }

    void MessageReaderCommandController::activeViewChanged()
    {
        if (m_findBarContainer.isVisible() && available())
        {
            m_plainTextFindQuery.clear();
            m_plainTextFindIndex = -1;
            runFind(false);
        }
        else if (!available())
        {
            updateFindResult(0, 0);
        }
    }

    void MessageReaderCommandController::applyZoom()
    {
        m_htmlView.setZoomFactor(std::pow(1.1, static_cast<double>(m_zoomSteps)));
    }

    void MessageReaderCommandController::zoomIn()
    {
        if (!available() || m_zoomSteps >= 15)
            return;
        ++m_zoomSteps;
        m_plainTextView.zoomIn(1);
        applyZoom();
    }

    void MessageReaderCommandController::zoomOut()
    {
        if (!available() || m_zoomSteps <= -8)
            return;
        --m_zoomSteps;
        m_plainTextView.zoomOut(1);
        applyZoom();
    }

    void MessageReaderCommandController::resetZoom()
    {
        if (!available())
            return;
        if (m_zoomSteps > 0)
            m_plainTextView.zoomOut(m_zoomSteps);
        else if (m_zoomSteps < 0)
            m_plainTextView.zoomIn(-m_zoomSteps);
        m_zoomSteps = 0;
        applyZoom();
    }

    void MessageReaderCommandController::printMessage()
    {
        if (!available())
            return;
        if (m_bodyPresenter.activeView() == MessageBodyPresenter::View::Html)
        {
            m_htmlView.printDocument(m_documentName());
            return;
        }

        QPrinter printer{QPrinter::HighResolution};
        printer.setDocName(m_documentName());
        QPrintDialog dialog{&printer, &m_dialogParent};
        if (dialog.exec() == QDialog::Accepted)
            m_plainTextView.print(&printer);
    }

    bool MessageReaderCommandController::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == &m_findEdit && event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
        {
            dismissFindBar();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

} // namespace javelin::gui::messageview
