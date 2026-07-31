#pragma once

#include <QUuid>

#include <cstdint>
#include <utility>

namespace javelin::app
{
    struct RefreshTicket
    {
        QUuid scope;
        std::uint64_t generation = 0;
        std::uint64_t causeEpoch = 0;
    };

    class RefreshGeneration final
    {
      public:
        explicit RefreshGeneration(QUuid scope = QUuid::createUuid()) : m_scope(std::move(scope))
        {
        }

        [[nodiscard]] RefreshTicket begin(std::uint64_t causeEpoch = 0)
        {
            return {.scope = m_scope, .generation = ++m_generation, .causeEpoch = causeEpoch};
        }

        void replaceScope(QUuid scope = QUuid::createUuid())
        {
            m_scope = std::move(scope);
            ++m_generation;
            m_installedEpoch = 0;
            m_closed = false;
        }

        void close()
        {
            m_closed = true;
            ++m_generation;
        }

        [[nodiscard]] bool accepts(const RefreshTicket& ticket,
                                   const std::uint64_t resultEpoch) const
        {
            return !m_closed && ticket.scope == m_scope && ticket.generation == m_generation &&
                   resultEpoch >= ticket.causeEpoch && resultEpoch >= m_installedEpoch;
        }

        [[nodiscard]] bool install(const RefreshTicket& ticket, const std::uint64_t resultEpoch)
        {
            if (!accepts(ticket, resultEpoch))
                return false;
            m_installedEpoch = resultEpoch;
            return true;
        }

        [[nodiscard]] const QUuid& scope() const
        {
            return m_scope;
        }

        [[nodiscard]] std::uint64_t generation() const
        {
            return m_generation;
        }

        [[nodiscard]] std::uint64_t installedEpoch() const
        {
            return m_installedEpoch;
        }

      private:
        QUuid m_scope;
        std::uint64_t m_generation = 0;
        std::uint64_t m_installedEpoch = 0;
        bool m_closed = false;
    };
} // namespace javelin::app
