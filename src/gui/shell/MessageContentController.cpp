#include "gui/shell/MessageContentController.h"

#include "app/MessageContentApplicationPorts.h"

#include <QCoroTask>

#include <QDebug>

#include <utility>
#include <variant>

namespace javelin::gui::shell
{
    MessageContentController::MessageContentController(
        javelin::app::MessageContentPort& contentPort, QObject* parent)
        : QObject(parent), m_contentPort(contentPort)
    {
    }

    void MessageContentController::request(std::string accountId, std::string emailId)
    {
        if (m_requestInFlight.has_value() && m_requestInFlight->accountId == accountId &&
            m_requestInFlight->emailId == emailId)
        {
            qDebug().noquote() << "GUI message content refresh already in flight"
                               << QString::fromStdString(emailId);
            return;
        }

        const auto requestToken = m_nextRequestToken++;
        m_requestInFlight = RequestState{
            .accountId = accountId,
            .emailId = emailId,
            .token = requestToken,
        };

        auto task = m_contentPort.requestMessageContent(accountId, emailId);
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(accountId), emailId = std::move(emailId),
             requestToken](javelin::jmap::MessageContentRefreshResult result)
            {
                const bool isCurrentRequest = m_requestInFlight.has_value() &&
                                              m_requestInFlight->token == requestToken &&
                                              m_requestInFlight->accountId == accountId &&
                                              m_requestInFlight->emailId == emailId;
                if (!isCurrentRequest)
                {
                    qDebug().noquote() << "GUI message content refresh ignored stale completion"
                                       << QString::fromStdString(emailId);
                    return;
                }

                m_requestInFlight.reset();
                if (const auto* unavailable =
                        std::get_if<javelin::jmap::MessageContentUnavailable>(&result))
                {
                    qWarning().noquote()
                        << "GUI message content unavailable" << unavailable->message;
                    Q_EMIT contentUnavailable(*unavailable);
                    return;
                }

                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    qWarning().noquote() << "GUI message content refresh failed" << error->message;
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto summary =
                    std::get<javelin::jmap::MessageContentRefreshSummary>(std::move(result));
                qInfo().noquote() << "GUI message content refresh succeeded"
                                  << QString::fromStdString(summary.emailId)
                                  << static_cast<qulonglong>(summary.partCount)
                                  << static_cast<qulonglong>(summary.bodyValueCount)
                                  << summary.usedCachedContent;
                Q_EMIT contentRefreshed(summary);
            });
    }
} // namespace javelin::gui::shell
