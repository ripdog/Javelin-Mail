#include "app/CacheAccessBarrier.h"

#include <algorithm>

namespace javelin::app
{
    CacheAccessBarrier::ParticipantId
    CacheAccessBarrier::registerParticipant(Participant participant)
    {
        const auto id = m_nextParticipantId++;
        if (m_suspended && participant.suspend)
            static_cast<void>(participant.suspend());
        m_participants.push_back({.id = id, .participant = std::move(participant)});
        return id;
    }

    void CacheAccessBarrier::unregisterParticipant(const ParticipantId id)
    {
        std::erase_if(m_participants,
                      [id](const auto& participant) { return participant.id == id; });
    }

    std::optional<javelin::jmap::cache::DatabaseError> CacheAccessBarrier::suspend()
    {
        if (m_suspended)
            return std::nullopt;

        std::vector<RegisteredParticipant*> suspended;
        suspended.reserve(m_participants.size());
        for (auto& participant : m_participants)
        {
            if (participant.participant.suspend)
            {
                if (const auto error = participant.participant.suspend())
                {
                    for (auto it = suspended.rbegin(); it != suspended.rend(); ++it)
                    {
                        if ((*it)->participant.resume)
                            static_cast<void>((*it)->participant.resume());
                    }
                    return error;
                }
            }
            suspended.push_back(&participant);
        }
        m_suspended = true;
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError> CacheAccessBarrier::resume()
    {
        if (!m_suspended)
            return std::nullopt;

        for (auto& participant : m_participants)
        {
            if (participant.participant.resume)
            {
                if (const auto error = participant.participant.resume())
                    return error;
            }
        }
        m_suspended = false;
        return std::nullopt;
    }

    bool CacheAccessBarrier::isSuspended() const
    {
        return m_suspended;
    }
} // namespace javelin::app
