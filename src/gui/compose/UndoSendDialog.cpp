#include "gui/compose/UndoSendDialog.h"

#include "app/ComposeApplicationPorts.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QDateTime>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace javelin::gui::compose
{
    UndoSendDialog::UndoSendDialog(QString sendId, QString title, QString message,
                                   const qint64 deadlineEpochMilliseconds,
                                   javelin::app::ComposeCommandPort& composeCommands,
                                   QWidget* parent)
        : QDialog(parent), m_sendId(std::move(sendId)),
          m_deadlineEpochMilliseconds(deadlineEpochMilliseconds),
          m_durationMilliseconds(
              std::max<qint64>(1, deadlineEpochMilliseconds - QDateTime::currentMSecsSinceEpoch())),
          m_composeCommands(composeCommands)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setModal(false);
        setWindowModality(Qt::NonModal);
        setWindowTitle(std::move(title));
        setMinimumWidth(360);

        auto* layout = new QVBoxLayout(this);
        auto* description = new QLabel(std::move(message), this);
        description->setWordWrap(true);
        layout->addWidget(description);

        m_countdown = new QLabel(this);
        m_countdown->setObjectName(QStringLiteral("undoSendCountdown"));
        layout->addWidget(m_countdown);

        m_progress = new QProgressBar(this);
        m_progress->setObjectName(QStringLiteral("undoSendProgress"));
        m_progress->setRange(0, static_cast<int>(std::min<qint64>(
                                    m_durationMilliseconds, std::numeric_limits<int>::max())));
        m_progress->setTextVisible(false);
        layout->addWidget(m_progress);

        m_undo = new QPushButton(i18nc("@action:button", "Undo Send"), this);
        m_undo->setObjectName(QStringLiteral("undoSendButton"));
        layout->addWidget(m_undo);
        connect(m_undo, &QPushButton::clicked, this, &UndoSendDialog::undoSend);

        m_timer.setInterval(50);
        m_timer.setTimerType(Qt::PreciseTimer);
        connect(&m_timer, &QTimer::timeout, this, &UndoSendDialog::updateCountdown);
        updateCountdown();
        if (QDateTime::currentMSecsSinceEpoch() < m_deadlineEpochMilliseconds)
            m_timer.start();
    }

    const QString& UndoSendDialog::sendId() const
    {
        return m_sendId;
    }

    void UndoSendDialog::updateCountdown()
    {
        const auto remaining =
            std::max<qint64>(0, m_deadlineEpochMilliseconds - QDateTime::currentMSecsSinceEpoch());
        const auto seconds = static_cast<int>(std::ceil(static_cast<double>(remaining) / 1000.0));
        m_countdown->setText(i18np("Sending in %1 second", "Sending in %1 seconds", seconds));
        m_progress->setValue(static_cast<int>(
            std::clamp<qint64>(m_durationMilliseconds - remaining, 0, m_progress->maximum())));
        if (remaining != 0)
            return;

        m_timer.stop();
        m_undo->setEnabled(false);
        m_countdown->setText(i18n("Sending…"));
        close();
    }

    void UndoSendDialog::undoSend()
    {
        if (m_cancelling)
            return;
        m_cancelling = true;
        m_undo->setEnabled(false);
        m_countdown->setText(i18n("Cancelling send…"));
        QCoro::connect(
            m_composeCommands.cancelDeferredSend(m_sendId), this,
            [this](std::variant<bool, javelin::jmap::OperationError> result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    m_countdown->setText(error->message);
                }
                else if (!std::get<bool>(result))
                {
                    m_countdown->setText(i18n("The message has already started sending."));
                }
                else
                {
                    close();
                    return;
                }

                m_cancelling = false;
                if (QDateTime::currentMSecsSinceEpoch() < m_deadlineEpochMilliseconds)
                {
                    m_undo->setEnabled(true);
                    m_timer.start();
                }
            });
    }
} // namespace javelin::gui::compose
