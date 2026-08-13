#pragma once

#include "app/MailApplicationEventsPorts.h"
#include "protocol/CacheContract.h"
#include "protocol/HandshakeContract.h"

namespace javelin::app
{
    class GuiDaemonSession;

    class GuiMailApplicationEvents final : public MailApplicationEventsPort
    {
        Q_OBJECT

      public:
        explicit GuiMailApplicationEvents(GuiDaemonSession& session, QObject* parent = nullptr);

        [[nodiscard]] std::unordered_map<std::string, MailAccountStatus>
        accountStatuses() const override;

      private:
        void applyStatus(const javelin::protocol::DaemonStatus& status);
        void publishInvalidation(const javelin::protocol::CacheInvalidation& invalidation);
        void publishThreadMaterializationProgress(
            const javelin::protocol::ThreadMaterializationProgress& progress);

        GuiDaemonSession& m_session;
        std::unordered_map<std::string, MailAccountStatus> m_statuses;
    };
} // namespace javelin::app
