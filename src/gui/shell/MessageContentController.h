#pragma once

#include "jmap/JmapCore.h"
#include "jmap/OperationError.h"

#include <QObject>

#include <cstdint>
#include <optional>
#include <string>

namespace javelin::app
{
    class MailApplicationService;
}

namespace javelin::gui::shell
{
    class MessageContentController final : public QObject
    {
        Q_OBJECT

      public:
        explicit MessageContentController(javelin::app::MailApplicationService& mailService,
                                          QObject* parent = nullptr);

        void request(std::string accountId, std::string emailId);

      Q_SIGNALS:
        void contentUnavailable(const javelin::jmap::MessageContentUnavailable& unavailable);
        void operationFailed(const javelin::jmap::OperationError& error);
        void contentRefreshed(const javelin::jmap::MessageContentRefreshSummary& summary);

      private:
        struct RequestState
        {
            std::string accountId;
            std::string emailId;
            std::uint64_t token = 0;
        };

        javelin::app::MailApplicationService& m_mailService;
        std::uint64_t m_nextRequestToken = 1;
        std::optional<RequestState> m_requestInFlight;
    };
} // namespace javelin::gui::shell
