#pragma once

#include "storage/DatabaseError.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace javelin::app
{

    class CacheAccessBarrier final
    {
      public:
        using ParticipantId = std::uint64_t;
        using Callback = std::function<std::optional<javelin::jmap::cache::DatabaseError>()>;

        struct Participant
        {
            QString name;
            Callback suspend;
            Callback resume;
        };

        [[nodiscard]] ParticipantId registerParticipant(Participant participant);
        void unregisterParticipant(ParticipantId id);

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError> suspend();
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError> resume();
        [[nodiscard]] bool isSuspended() const;

      private:
        struct RegisteredParticipant
        {
            ParticipantId id = 0;
            Participant participant;
        };

        std::vector<RegisteredParticipant> m_participants;
        ParticipantId m_nextParticipantId = 1;
        bool m_suspended = false;
    };

} // namespace javelin::app
