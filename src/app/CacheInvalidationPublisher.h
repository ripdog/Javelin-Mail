#pragma once

#include "app/MailApplicationTypes.h"

#include <QObject>
#include <QTimer>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace javelin::app
{
    class CacheInvalidationPublisher final : public QObject
    {
        Q_OBJECT

      public:
        explicit CacheInvalidationPublisher(QObject* parent = nullptr);

        void publish(MailCacheChange change);
        void flush();

        [[nodiscard]] std::uint64_t currentEpoch() const;

      Q_SIGNALS:
        void invalidated(javelin::app::MailCacheInvalidation invalidation);

      private:
        static void merge(MailCacheChange& target, MailCacheChange source);
        [[nodiscard]] static std::vector<javelin::protocol::ChangedDomain>
        changedDomains(const MailCacheChange& change);
        [[nodiscard]] static std::vector<QString> affectedKeys(const MailCacheChange& change);

        QTimer m_flushTimer;
        std::deque<MailCacheChange> m_pending;
        std::uint64_t m_epoch = 0;
    };
} // namespace javelin::app
