#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::app::undo
{

    enum class HistoryStack
    {
        Undo,
        Redo,
    };

    enum class HistoryEntryStatus
    {
        Preparing,
        ExecutingForward,
        Ready,
        ExecutingUndo,
        ExecutingRedo,
        BlockedUnknown,
        BlockedPartial,
        Impossible,
        Expired,
    };

    enum class CommandOrigin
    {
        User,
        Undo,
        Redo,
        SystemChild,
    };

    enum class HistoryDomain
    {
        Mail,
        Calendar,
        Contacts,
        LocalPreference,
        DeferredSend,
    };

    struct ExactMailPatch
    {
        std::vector<std::string> addMailboxIds;
        std::vector<std::string> removeMailboxIds;
        std::vector<std::string> addKeywords;
        std::vector<std::string> removeKeywords;

        auto operator<=>(const ExactMailPatch&) const = default;
    };

    struct MailPatchItemHistory
    {
        std::string accountId;
        std::string emailId;
        std::optional<std::string> subject;
        ExactMailPatch forward;
        ExactMailPatch inverse;
        ExactMailPatch expectedBefore;
        ExactMailPatch expectedAfter;
        std::optional<std::string> mutationId;

        auto operator<=>(const MailPatchItemHistory&) const = default;
    };

    struct MailPatchHistory
    {
        std::vector<MailPatchItemHistory> items;
        auto operator<=>(const MailPatchHistory&) const = default;
    };

    enum class MailTransferHistoryOperation
    {
        Copy,
        Move,
    };

    struct MailTransferItemHistory
    {
        std::optional<std::string> currentSourceEmailId;
        std::vector<std::string> originalSourceMailboxIds;
        std::vector<std::string> sourceKeywords;
        std::optional<std::string> sourceReceivedAt;
        std::uint64_t sourceSize = 0;
        std::vector<std::string> sourceRemovedMailboxIds;
        bool sourceDestroyed = false;
        std::optional<std::string> rawContentHash;
        std::optional<std::string> currentDestinationEmailId;
        bool destinationReusedExisting = false;
        std::vector<std::string> destinationPriorMailboxIds;
        std::vector<std::string> destinationMailboxIds;
        std::vector<std::string> destinationKeywords;

        auto operator<=>(const MailTransferItemHistory&) const = default;
    };

    struct MailTransferHistory
    {
        std::string sourceAccountId;
        std::string destinationAccountId;
        std::string destinationMailboxId;
        MailTransferHistoryOperation operation = MailTransferHistoryOperation::Copy;
        std::vector<MailTransferItemHistory> items;

        auto operator<=>(const MailTransferHistory&) const = default;
    };

    struct DraftHistory
    {
        std::string connectionId;
        std::string accountId;
        std::string composeSessionId;
        std::optional<std::string> currentDraftEmailId;
        std::optional<std::string> beforeSnapshotJson;
        std::string afterSnapshotJson;

        auto operator<=>(const DraftHistory&) const = default;
    };

    struct SieveHistory
    {
        std::string connectionId;
        std::string accountId;
        std::optional<std::string> currentScriptId;
        std::optional<std::string> previousScriptId;
        std::optional<std::string> beforeName;
        std::optional<std::string> beforeContent;
        std::optional<std::string> afterName;
        std::optional<std::string> afterContent;
        std::optional<std::string> activeScriptIdBefore;
        std::optional<std::string> activeScriptIdAfter;

        auto operator<=>(const SieveHistory&) const = default;
    };

    struct DeferredSendHistory
    {
        std::string sendId;
        std::string connectionId;
        std::string accountId;
        std::string composeSessionId;
        std::string draftEmailId;
        std::optional<std::string> subject;
        std::int64_t delaySeconds = 10;

        auto operator<=>(const DeferredSendHistory&) const = default;
    };

    struct CalendarEventHistory
    {
        std::string connectionId;
        std::string accountId;
        std::string calendarId;
        std::optional<std::string> currentEventId;
        std::string uid;
        std::optional<std::string> beforeDocumentJson;
        std::optional<std::string> afterDocumentJson;

        auto operator<=>(const CalendarEventHistory&) const = default;
    };

    struct CalendarPreferenceHistory
    {
        std::string connectionId;
        std::string accountId;
        std::string preferenceKind;
        std::string objectId;
        std::optional<std::string> beforeValue;
        std::optional<std::string> afterValue;

        auto operator<=>(const CalendarPreferenceHistory&) const = default;
    };

    struct ContactCardItemHistory
    {
        std::string addressBookId;
        std::optional<std::string> currentCardId;
        std::string uid;
        std::optional<std::string> beforeDocumentJson;
        std::optional<std::string> afterDocumentJson;

        auto operator<=>(const ContactCardItemHistory&) const = default;
    };

    struct ContactCardHistory
    {
        std::string connectionId;
        std::string accountId;
        std::vector<ContactCardItemHistory> items;

        auto operator<=>(const ContactCardHistory&) const = default;
    };

    struct AddressBookHistory
    {
        std::string connectionId;
        std::string accountId;
        std::optional<std::string> currentAddressBookId;
        std::optional<std::string> beforeDocumentJson;
        std::optional<std::string> afterDocumentJson;
        std::optional<std::string> beforeDefaultAddressBookId;
        std::optional<std::string> afterDefaultAddressBookId;
        std::vector<ContactCardItemHistory> affectedCards;

        auto operator<=>(const AddressBookHistory&) const = default;
    };

    struct ContactGroupHistory
    {
        ContactCardItemHistory group;
        std::vector<std::string> beforeMemberUids;
        std::vector<std::string> afterMemberUids;

        auto operator<=>(const ContactGroupHistory&) const = default;
    };

    struct ImpossibleHistory
    {
        std::string explanation;
        auto operator<=>(const ImpossibleHistory&) const = default;
    };

    using HistoryPayload =
        std::variant<MailPatchHistory, MailTransferHistory, DraftHistory, SieveHistory, DeferredSendHistory,
                     CalendarEventHistory, CalendarPreferenceHistory, ContactCardHistory,
                     AddressBookHistory, ContactGroupHistory, ImpossibleHistory>;

    struct HistoryEntry
    {
        QString entryId;
        HistoryStack stack = HistoryStack::Undo;
        std::int64_t stackOrder = 0;
        QString label;
        HistoryDomain domain = HistoryDomain::Mail;
        QString commandKind;
        int payloadVersion = 1;
        HistoryPayload payload;
        HistoryEntryStatus status = HistoryEntryStatus::Preparing;
        std::optional<QString> operationGroupId;
        std::optional<QDateTime> expiresAt;
        std::optional<QString> explanation;
        std::optional<QString> failureJson;
        QDateTime createdAt;
        QDateTime updatedAt;
    };

    struct HistoryState
    {
        QString undoLabel;
        QString redoLabel;
        bool canUndo = false;
        bool canRedo = false;
        bool executing = false;
        bool blocked = false;
    };

    struct HistoryObjectFailure
    {
        QString objectId;
        QString summary;
    };

    struct HistoryFailure
    {
        QString entryId;
        QString actionLabel;
        QString summary;
        std::vector<HistoryObjectFailure> objectFailures;
        bool mayRemoveFromHistory = false;
        bool acknowledgeAndRemove = false;
    };

    [[nodiscard]] QString toString(HistoryStack value);
    [[nodiscard]] std::optional<HistoryStack> historyStackFromString(const QString& value);
    [[nodiscard]] QString toString(HistoryEntryStatus value);
    [[nodiscard]] std::optional<HistoryEntryStatus>
    historyEntryStatusFromString(const QString& value);
    [[nodiscard]] QString toString(HistoryDomain value);
    [[nodiscard]] std::optional<HistoryDomain> historyDomainFromString(const QString& value);

} // namespace javelin::app::undo

Q_DECLARE_METATYPE(javelin::app::undo::HistoryState)
Q_DECLARE_METATYPE(javelin::app::undo::HistoryFailure)
