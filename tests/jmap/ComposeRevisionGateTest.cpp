#include "jmap/submission/ComposeRevisionGate.h"

#include <catch2/catch_test_macros.hpp>

using namespace javelin::jmap::submission;

namespace
{
    [[nodiscard]] DraftSnapshot snapshot(std::uint64_t revision,
                                         std::vector<DraftAttachment> attachments = {})
    {
        return {
            .composeSessionId = "compose-1",
            .accountId = "account-1",
            .revision = revision,
            .draftEmailId = std::nullopt,
            .mode = ComposeMode::NewMessage,
            .editorMode = BodyEditorMode::RichText,
            .identityId = "identity-1",
            .to = {},
            .cc = {},
            .bcc = {},
            .subject = std::nullopt,
            .plainTextBody = "body",
            .htmlBody = "<p>body</p>",
            .threading = {},
            .attachments = std::move(attachments),
        };
    }
} // namespace

TEST_CASE("compose revision gate rejects an older save after a newer revision",
          "[jmap][submission][compose][revision]")
{
    ComposeRevisionGate gate;
    const auto first = snapshot(4);
    const auto newer = snapshot(5);

    CHECK(gate.admit(first.composeSessionId, first.revision, acceptedAttachmentManifest(first)) ==
          ComposeRevisionAdmission::Accepted);
    CHECK(gate.accept(first.composeSessionId, first.revision, acceptedAttachmentManifest(first)));
    CHECK(gate.admit(newer.composeSessionId, newer.revision, acceptedAttachmentManifest(newer)) ==
          ComposeRevisionAdmission::Accepted);
    CHECK(gate.accept(newer.composeSessionId, newer.revision, acceptedAttachmentManifest(newer)));
    CHECK(gate.admit(first.composeSessionId, first.revision, acceptedAttachmentManifest(first)) ==
          ComposeRevisionAdmission::Stale);
    CHECK_FALSE(
        gate.isAccepted(first.composeSessionId, first.revision, acceptedAttachmentManifest(first)));
    CHECK(
        gate.isAccepted(newer.composeSessionId, newer.revision, acceptedAttachmentManifest(newer)));
}

TEST_CASE("compose revision gate binds an accepted revision to its attachment manifest",
          "[jmap][submission][compose][revision]")
{
    ComposeRevisionGate gate;
    auto first = snapshot(1, {DraftAttachment{
                                 .localFilePath = {},
                                 .displayName = "report.pdf",
                                 .mediaType = "application/pdf",
                                 .size = 42,
                                 .blobId = "blob-1",
                                 .inlineDisposition = false,
                                 .contentId = std::nullopt,
                                 .contentHash = "hash-1",
                             }});
    auto changed = first;
    changed.attachments.front().contentHash = "hash-2";

    const auto manifest = acceptedAttachmentManifest(first);
    CHECK(gate.admit(first.composeSessionId, first.revision, manifest) ==
          ComposeRevisionAdmission::Accepted);
    CHECK(gate.accept(first.composeSessionId, first.revision, manifest));
    CHECK(gate.admit(changed.composeSessionId, changed.revision,
                     acceptedAttachmentManifest(changed)) ==
          ComposeRevisionAdmission::ManifestChanged);
}

TEST_CASE("compose snapshot bounds reject oversized bodies and attachment sets",
          "[jmap][submission][compose][revision]")
{
    auto body = snapshot(1);
    body.plainTextBody.assign(maximumComposeBodyBytes + 1, 'x');
    CHECK_FALSE(isBoundedComposeSnapshot(body));

    auto attachment = DraftAttachment{
        .localFilePath = {},
        .displayName = "large.bin",
        .mediaType = "application/octet-stream",
        .size = maximumComposeAttachmentBytes + 1,
        .blobId = std::nullopt,
        .inlineDisposition = false,
        .contentId = std::nullopt,
        .contentHash = std::nullopt,
    };
    body = snapshot(1, {std::move(attachment)});
    CHECK_FALSE(isBoundedComposeSnapshot(body));
}
