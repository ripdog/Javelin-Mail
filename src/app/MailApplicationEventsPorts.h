#pragma once

#include "app/MailApplicationTypes.h"

#include <QObject>

#include <string>
#include <unordered_map>

namespace javelin::app
{
    enum class MailAccountStatus
    {
        Disconnected,
        Connecting,
        Connected,
        AuthenticationPaused,
    };

    class MailApplicationEventsPort : public QObject
    {
        Q_OBJECT

      public:
        explicit MailApplicationEventsPort(QObject* parent = nullptr) : QObject(parent)
        {
        }
        ~MailApplicationEventsPort() override = default;

        [[nodiscard]] virtual std::unordered_map<std::string, MailAccountStatus>
        accountStatuses() const = 0;

      Q_SIGNALS:
        void accountStatusChanged(const QString& accountId, MailAccountStatus status);
        void sessionCapabilitiesChanged(const QString& ownerAccountId);
        void cacheInvalidated(javelin::app::MailCacheInvalidation invalidation);
        void threadMaterializationProgress(javelin::app::ThreadMaterializationProgress progress);
    };
} // namespace javelin::app
