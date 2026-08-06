#pragma once

#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

namespace javelin::app
{
    class MailboxMaintenanceRegistry final
    {
      public:
        class Lease final
        {
          public:
            Lease() = default;
            ~Lease();

            Lease(const Lease&) = delete;
            Lease& operator=(const Lease&) = delete;
            Lease(Lease&& other) noexcept;
            Lease& operator=(Lease&& other) noexcept;

            [[nodiscard]] explicit operator bool() const;
            [[nodiscard]] std::uint64_t generation() const;

          private:
            friend class MailboxMaintenanceRegistry;

            Lease(MailboxMaintenanceRegistry& registry, QString key, std::uint64_t generation);
            void release();

            MailboxMaintenanceRegistry* m_registry = nullptr;
            QString m_key;
            std::uint64_t m_generation = 0;
        };

        [[nodiscard]] std::optional<Lease> tryBegin(const QString& accountId,
                                                    const QString& mailboxId);
        [[nodiscard]] bool isActive(const QString& accountId, const QString& mailboxId) const;
        [[nodiscard]] bool isActiveForEmail(const QString& accountId,
                                            const QStringList& mailboxIds) const;

      private:
        struct Entry
        {
            bool active = false;
            std::uint64_t generation = 0;
        };

        [[nodiscard]] static QString key(const QString& accountId, const QString& mailboxId);
        void release(const QString& key);

        mutable QMutex m_mutex;
        QHash<QString, Entry> m_entries;
    };
} // namespace javelin::app
