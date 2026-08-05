#include "gui/logging/LogViewerDialog.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>

namespace javelin::gui::logging
{
    namespace
    {
        int severity(const QtMsgType type)
        {
            switch (type)
            {
            case QtDebugMsg:
                return 0;
            case QtInfoMsg:
                return 1;
            case QtWarningMsg:
                return 2;
            case QtCriticalMsg:
                return 3;
            case QtFatalMsg:
                return 4;
            }
            return 0;
        }
    } // namespace

    LogViewerDialog::LogViewerDialog(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle(i18n("Application Log"));
        resize(1000, 650);
        auto* layout = new QVBoxLayout(this);
        auto* filters = new QHBoxLayout;
        m_level = new QComboBox(this);
        m_level->addItems({i18n("Debug and above"), i18n("Info and above"),
                           i18n("Warnings and above"), i18n("Errors only")});
        m_level->setCurrentIndex(1);
        m_subsystem = new QComboBox(this);
        m_subsystem->addItem(i18n("All systems"));
        m_search = new QLineEdit(this);
        m_search->setPlaceholderText(i18n("Filter log text"));
        filters->addWidget(m_level);
        filters->addWidget(m_subsystem);
        filters->addWidget(m_search, 1);
        layout->addLayout(filters);
        m_output = new QPlainTextEdit(this);
        m_output->setReadOnly(true);
        m_output->setLineWrapMode(QPlainTextEdit::NoWrap);
        layout->addWidget(m_output, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        auto* clear =
            buttons->addButton(i18nc("@action:button", "Clear"), QDialogButtonBox::ResetRole);
        connect(clear, &QPushButton::clicked, &javelin::app::LogStore::instance(),
                &javelin::app::LogStore::clear);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        connect(m_level, &QComboBox::currentIndexChanged, this, &LogViewerDialog::rebuild);
        connect(m_subsystem, &QComboBox::currentIndexChanged, this, &LogViewerDialog::rebuild);
        connect(m_search, &QLineEdit::textChanged, this, &LogViewerDialog::rebuild);
        connect(&javelin::app::LogStore::instance(), &javelin::app::LogStore::entryAdded, this,
                &LogViewerDialog::append, Qt::QueuedConnection);
        connect(&javelin::app::LogStore::instance(), &javelin::app::LogStore::cleared, m_output,
                &QPlainTextEdit::clear);
        rebuild();
    }

    bool LogViewerDialog::accepts(const javelin::app::LogEntry& entry) const
    {
        return severity(entry.level) >= m_level->currentIndex() &&
               (m_subsystem->currentIndex() == 0 ||
                m_subsystem->currentText() == entry.subsystem) &&
               (m_search->text().isEmpty() ||
                entry.message.contains(m_search->text(), Qt::CaseInsensitive));
    }

    void LogViewerDialog::append(const javelin::app::LogEntry& entry)
    {
        if (m_subsystem->findText(entry.subsystem) < 0)
        {
            m_subsystem->addItem(entry.subsystem);
        }
        if (!accepts(entry))
            return;
        QTextCharFormat format;
        format.setForeground(entry.level == QtDebugMsg     ? QColor{Qt::gray}
                             : entry.level == QtInfoMsg    ? palette().color(QPalette::Text)
                             : entry.level == QtWarningMsg ? QColor{QStringLiteral("#d98e04")}
                                                           : QColor{QStringLiteral("#d63031")});
        if (severity(entry.level) >= severity(QtWarningMsg))
            format.setFontWeight(QFont::Bold);
        QTextCursor cursor = m_output->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(
            QStringLiteral("%1  %-5s  [%2]  %3\n")
                .arg(entry.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                     entry.subsystem, entry.message)
                .replace(QStringLiteral("%-5s"),
                         entry.level == QtDebugMsg     ? QStringLiteral("DEBUG")
                         : entry.level == QtInfoMsg    ? QStringLiteral("INFO ")
                         : entry.level == QtWarningMsg ? QStringLiteral("WARN ")
                                                       : QStringLiteral("ERROR")),
            format);
        m_output->setTextCursor(cursor);
    }

    void LogViewerDialog::rebuild()
    {
        m_output->clear();
        const auto entries = javelin::app::LogStore::instance().entries();
        for (const auto& entry : entries)
        {
            if (m_subsystem->findText(entry.subsystem) < 0)
                m_subsystem->addItem(entry.subsystem);
            append(entry);
        }
    }
} // namespace javelin::gui::logging
