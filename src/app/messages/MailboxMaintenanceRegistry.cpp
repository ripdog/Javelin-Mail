#include "app/MailboxMaintenanceRegistry.h"

#include <QMutexLocker>

#include <utility>

namespace javelin::app
{
    MailboxMaintenanceRegistry::Lease::Lease(MailboxMaintenanceRegistry& registry, QString key,
                                             const std::uint64_t generation)
        : m_registry(&registry), m_key(std::move(key)), m_generation(generation)
    {
    }

    MailboxMaintenanceRegistry::Lease::~Lease()
    {
        release();
    }

    MailboxMaintenanceRegistry::Lease::Lease(Lease&& other) noexcept
        : m_registry(std::exchange(other.m_registry, nullptr)), m_key(std::move(other.m_key)),
          m_generation(std::exchange(other.m_generation, 0))
    {
    }

    MailboxMaintenanceRegistry::Lease&
    MailboxMaintenanceRegistry::Lease::operator=(Lease&& other) noexcept
    {
        if (this == &other)
            return *this;
        release();
        m_registry = std::exchange(other.m_registry, nullptr);
        m_key = std::move(other.m_key);
        m_generation = std::exchange(other.m_generation, 0);
        return *this;
    }

    MailboxMaintenanceRegistry::Lease::operator bool() const
    {
        return m_registry != nullptr;
    }

    std::uint64_t MailboxMaintenanceRegistry::Lease::generation() const
    {
        return m_generation;
    }

    void MailboxMaintenanceRegistry::Lease::release()
    {
        if (m_registry == nullptr)
            return;
        m_registry->release(m_key);
        m_registry = nullptr;
        m_key.clear();
        m_generation = 0;
    }

    std::optional<MailboxMaintenanceRegistry::Lease>
    MailboxMaintenanceRegistry::tryBegin(const QString& accountId, const QString& mailboxId)
    {
        const QString targetKey = key(accountId, mailboxId);
        QMutexLocker lock{&m_mutex};
        auto& entry = m_entries[targetKey];
        if (entry.active)
            return std::nullopt;
        entry.active = true;
        ++entry.generation;
        return Lease{*this, targetKey, entry.generation};
    }

    bool MailboxMaintenanceRegistry::isActive(const QString& accountId,
                                              const QString& mailboxId) const
    {
        const QString targetKey = key(accountId, mailboxId);
        QMutexLocker lock{&m_mutex};
        const auto found = m_entries.constFind(targetKey);
        return found != m_entries.constEnd() && found->active;
    }

    bool MailboxMaintenanceRegistry::isActiveForEmail(const QString& accountId,
                                                      const QStringList& mailboxIds) const
    {
        QMutexLocker lock{&m_mutex};
        for (const auto& mailboxId : mailboxIds)
        {
            const auto found = m_entries.constFind(key(accountId, mailboxId));
            if (found != m_entries.constEnd() && found->active)
                return true;
        }
        return false;
    }

    QString MailboxMaintenanceRegistry::key(const QString& accountId, const QString& mailboxId)
    {
        return accountId + QLatin1Char('\n') + mailboxId;
    }

    void MailboxMaintenanceRegistry::release(const QString& targetKey)
    {
        QMutexLocker lock{&m_mutex};
        const auto found = m_entries.find(targetKey);
        if (found != m_entries.end())
            found->active = false;
    }
} // namespace javelin::app
