#pragma once

#include "jmap/submission/ComposeTypes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace javelin::jmap::submission
{

    inline constexpr std::size_t maximumComposeRecipients = 256;
    inline constexpr std::size_t maximumComposeAttachments = 128;
    inline constexpr std::size_t maximumComposeBodyBytes = 8U * 1024U * 1024U;
    inline constexpr std::uint64_t maximumComposeAttachmentBytes = 512U * 1024U * 1024U;

    [[nodiscard]] inline bool isBoundedComposeSnapshot(const DraftSnapshot& snapshot)
    {
        const auto recipientCount = snapshot.to.size() + snapshot.cc.size() + snapshot.bcc.size();
        if (recipientCount > maximumComposeRecipients ||
            snapshot.attachments.size() > maximumComposeAttachments ||
            snapshot.plainTextBody.size() > maximumComposeBodyBytes ||
            snapshot.htmlBody.size() > maximumComposeBodyBytes)
            return false;

        std::uint64_t attachmentBytes = 0;
        for (const auto& attachment : snapshot.attachments)
        {
            if (attachment.size > maximumComposeAttachmentBytes ||
                attachmentBytes > maximumComposeAttachmentBytes - attachment.size)
                return false;
            attachmentBytes += attachment.size;
        }
        return true;
    }

    enum class ComposeRevisionAdmission
    {
        Accepted,
        Stale,
        ManifestChanged,
    };

    [[nodiscard]] inline std::vector<DraftAttachment>
    acceptedAttachmentManifest(const DraftSnapshot& snapshot)
    {
        std::vector<DraftAttachment> manifest;
        manifest.reserve(snapshot.attachments.size());
        for (const auto& attachment : snapshot.attachments)
        {
            manifest.push_back(DraftAttachment{
                .localFilePath = {},
                .displayName = attachment.displayName,
                .mediaType = attachment.mediaType,
                .size = attachment.size,
                .blobId = attachment.blobId,
                .inlineDisposition = attachment.inlineDisposition,
                .contentId = attachment.contentId,
                .contentHash = attachment.contentHash,
            });
        }
        return manifest;
    }

    [[nodiscard]] inline bool sameAcceptedAttachment(const DraftAttachment& left,
                                                     const DraftAttachment& right)
    {
        const auto& leftIdentity = left.contentHash.has_value() ? left.contentHash : left.blobId;
        const auto& rightIdentity =
            right.contentHash.has_value() ? right.contentHash : right.blobId;
        return leftIdentity == rightIdentity && left.displayName == right.displayName &&
               left.mediaType == right.mediaType && left.size == right.size &&
               left.inlineDisposition == right.inlineDisposition &&
               left.contentId == right.contentId;
    }

    [[nodiscard]] inline bool sameAcceptedManifest(const std::vector<DraftAttachment>& left,
                                                   const std::vector<DraftAttachment>& right)
    {
        if (left.size() != right.size())
            return false;
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            if (!sameAcceptedAttachment(left[index], right[index]))
                return false;
        }
        return true;
    }

    class ComposeRevisionGate final
    {
      public:
        [[nodiscard]] ComposeRevisionAdmission admit(const std::string& composeSessionId,
                                                     std::uint64_t revision,
                                                     const std::vector<DraftAttachment>& manifest)
        {
            auto& state = m_sessions[composeSessionId];
            if (revision < state.latestRequested || revision < state.latestAccepted)
                return ComposeRevisionAdmission::Stale;
            if (revision == state.latestAccepted && state.acceptedManifest.has_value() &&
                !sameAcceptedManifest(*state.acceptedManifest, manifest))
                return ComposeRevisionAdmission::ManifestChanged;
            state.latestRequested = std::max(state.latestRequested, revision);
            return ComposeRevisionAdmission::Accepted;
        }

        [[nodiscard]] bool accept(const std::string& composeSessionId, std::uint64_t revision,
                                  std::vector<DraftAttachment> manifest)
        {
            auto& state = m_sessions[composeSessionId];
            if (revision < state.latestRequested || revision < state.latestAccepted)
                return false;
            if (revision == state.latestAccepted && state.acceptedManifest.has_value() &&
                !sameAcceptedManifest(*state.acceptedManifest, manifest))
                return false;
            state.latestAccepted = revision;
            state.latestRequested = std::max(state.latestRequested, revision);
            state.acceptedManifest = std::move(manifest);
            return true;
        }

        [[nodiscard]] bool isAccepted(const std::string& composeSessionId, std::uint64_t revision,
                                      const std::vector<DraftAttachment>& manifest) const
        {
            const auto found = m_sessions.find(composeSessionId);
            if (found == m_sessions.end() || found->second.latestAccepted != revision ||
                !found->second.acceptedManifest.has_value())
                return false;
            return sameAcceptedManifest(*found->second.acceptedManifest, manifest);
        }

        [[nodiscard]] bool restoreAccepted(const std::string& composeSessionId,
                                           std::uint64_t revision,
                                           std::vector<DraftAttachment> manifest)
        {
            if (m_sessions.contains(composeSessionId))
                return false;
            auto& state = m_sessions[composeSessionId];
            state.latestRequested = revision;
            state.latestAccepted = revision;
            state.acceptedManifest = std::move(manifest);
            return true;
        }

        void forget(const std::string_view composeSessionId)
        {
            m_sessions.erase(std::string{composeSessionId});
        }

      private:
        struct SessionState
        {
            std::uint64_t latestRequested = 0;
            std::uint64_t latestAccepted = 0;
            std::optional<std::vector<DraftAttachment>> acceptedManifest;
        };

        std::unordered_map<std::string, SessionState> m_sessions;
    };

} // namespace javelin::jmap::submission
