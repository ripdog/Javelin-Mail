#pragma once

#include "jmap/domain/MailEntities.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace javelin::jmap::submission
{

    enum class ComposeMode
    {
        NewMessage,
        Reply,
        ReplyAll,
        Forward,
        EditDraft,
    };

    enum class BodyEditorMode
    {
        RichText,
        RawHtml,
        PlainText,
    };

    struct DraftAttachment
    {
        std::string localFilePath;
        std::string displayName;
        std::string mediaType;
        std::uint64_t size = 0;
        std::optional<std::string> blobId;
        bool inlineDisposition = false;
        std::optional<std::string> contentId;
        std::optional<std::string> contentHash;
    };

    struct ThreadingContext
    {
        std::vector<std::string> messageId;
        std::vector<std::string> inReplyTo;
        std::vector<std::string> references;
    };

    struct DraftSnapshot
    {
        std::string composeSessionId;
        std::string accountId;
        std::uint64_t revision = 1;
        std::optional<std::string> draftEmailId;
        ComposeMode mode = ComposeMode::NewMessage;
        BodyEditorMode editorMode = BodyEditorMode::RichText;
        std::string identityId;
        std::vector<javelin::jmap::domain::EmailAddress> to;
        std::vector<javelin::jmap::domain::EmailAddress> cc;
        std::vector<javelin::jmap::domain::EmailAddress> bcc;
        std::optional<std::string> subject;
        std::string plainTextBody;
        std::string htmlBody;
        ThreadingContext threading;
        std::vector<DraftAttachment> attachments;
    };

    struct OpenComposeRequest
    {
        std::string accountId;
        ComposeMode mode = ComposeMode::NewMessage;
        std::optional<std::string> referenceEmailId;
        std::optional<std::string> draftEmailId;
        std::vector<javelin::jmap::domain::EmailAddress> initialTo;
        bool useExistingWorkingCopy = true;
        std::optional<std::string> composeSessionId = std::nullopt;
    };

    struct DraftSaveSummary
    {
        std::string composeSessionId;
        std::string accountId;
        std::string draftEmailId;
        std::vector<std::string> affectedMailboxIds;
        std::string operationGroupId;
        std::string createMutationId;
        std::optional<std::string> destroyMutationId;
        std::uint64_t acceptedRevision = 1;
        std::vector<DraftAttachment> acceptedManifest;
        DraftSnapshot savedSnapshot;
    };

    struct DraftDeleteSummary
    {
        std::string accountId;
        std::string draftEmailId;
        std::string operationGroupId;
        std::string mutationId;
    };

    struct SendSummary
    {
        std::string composeSessionId;
        std::string accountId;
        std::string draftEmailId;
        std::optional<std::string> submissionId;
        std::uint64_t acceptedRevision = 1;
        bool scheduled = false;
    };

    struct PreparedSend
    {
        DraftSaveSummary draft;
        std::uint64_t acceptedRevision = 1;
        std::vector<DraftAttachment> acceptedManifest;
    };

    [[nodiscard]] inline std::string_view toString(const ComposeMode mode)
    {
        switch (mode)
        {
        case ComposeMode::NewMessage:
            return "new";
        case ComposeMode::Reply:
            return "reply";
        case ComposeMode::ReplyAll:
            return "reply_all";
        case ComposeMode::Forward:
            return "forward";
        case ComposeMode::EditDraft:
            return "edit_draft";
        }

        return "new";
    }

    [[nodiscard]] inline std::optional<ComposeMode> composeModeFromString(std::string_view value)
    {
        if (value == "new")
        {
            return ComposeMode::NewMessage;
        }
        if (value == "reply")
        {
            return ComposeMode::Reply;
        }
        if (value == "reply_all")
        {
            return ComposeMode::ReplyAll;
        }
        if (value == "forward")
        {
            return ComposeMode::Forward;
        }
        if (value == "edit_draft")
        {
            return ComposeMode::EditDraft;
        }

        return std::nullopt;
    }

    [[nodiscard]] inline std::string_view toString(const BodyEditorMode mode)
    {
        switch (mode)
        {
        case BodyEditorMode::RichText:
            return "rich_text";
        case BodyEditorMode::RawHtml:
            return "raw_html";
        case BodyEditorMode::PlainText:
            return "plain_text";
        }

        return "rich_text";
    }

    [[nodiscard]] inline std::optional<BodyEditorMode>
    bodyEditorModeFromString(std::string_view value)
    {
        if (value == "rich_text")
        {
            return BodyEditorMode::RichText;
        }
        if (value == "raw_html")
        {
            return BodyEditorMode::RawHtml;
        }
        if (value == "plain_text")
        {
            return BodyEditorMode::PlainText;
        }

        return std::nullopt;
    }

} // namespace javelin::jmap::submission
