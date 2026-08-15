#pragma once

#include "storage/DatabaseError.h"

#include <QMetaObject>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::app
{
    enum class WorkKind
    {
        FullMailSync,
        MessageDownload,
        SearchIndex,
        LegacyMigration,
        VaultProjection,
        ContactRefresh,
        CalendarRefresh,
        TagDeletion,
        Maintenance,
        MailTransfer,
    };

    enum class WorkPriority : int
    {
        Maintenance = 100,
        Derived = 200,
        Bulk = 300,
        Freshness = 400,
        Foreground = 500,
        VisibleMaterialization = 550,
        Interactive = 600,
    };

    enum class WorkStatus
    {
        Queued,
        Running,
        Paused,
        WaitingForSpace,
        WaitingForNetwork,
        WaitingForAuth,
        Failed,
        Complete,
    };

    struct WorkProgress
    {
        std::uint64_t completedUnits = 0;
        std::optional<std::uint64_t> totalUnits;
        std::uint64_t completedBytes = 0;
        std::optional<std::uint64_t> totalBytes;
        QString detail;
    };

    struct WorkRecord
    {
        std::string jobId;
        std::optional<std::string> parentJobId;
        std::optional<std::string> accountId;
        WorkKind kind = WorkKind::Maintenance;
        WorkPriority priority = WorkPriority::Maintenance;
        WorkStatus status = WorkStatus::Queued;
        QString title;
        WorkProgress progress;
        QString checkpointJson = QStringLiteral("{}");
        std::optional<QString> errorText;
        bool pauseRequested = false;
    };

    [[nodiscard]] constexpr std::string_view toString(const WorkKind kind)
    {
        switch (kind)
        {
        case WorkKind::FullMailSync:
            return "full_mail_sync";
        case WorkKind::MessageDownload:
            return "message_download";
        case WorkKind::SearchIndex:
            return "search_index";
        case WorkKind::LegacyMigration:
            return "legacy_migration";
        case WorkKind::VaultProjection:
            return "vault_projection";
        case WorkKind::ContactRefresh:
            return "contact_refresh";
        case WorkKind::CalendarRefresh:
            return "calendar_refresh";
        case WorkKind::TagDeletion:
            return "tag_deletion";
        case WorkKind::Maintenance:
            return "maintenance";
        case WorkKind::MailTransfer:
            return "mail_transfer";
        }
        return "maintenance";
    }

    [[nodiscard]] constexpr std::string_view toString(const WorkStatus status)
    {
        switch (status)
        {
        case WorkStatus::Queued:
            return "queued";
        case WorkStatus::Running:
            return "running";
        case WorkStatus::Paused:
            return "paused";
        case WorkStatus::WaitingForSpace:
            return "waiting_for_space";
        case WorkStatus::WaitingForNetwork:
            return "waiting_for_network";
        case WorkStatus::WaitingForAuth:
            return "waiting_for_auth";
        case WorkStatus::Failed:
            return "failed";
        case WorkStatus::Complete:
            return "complete";
        }
        return "queued";
    }

    class WorkTaskPort
    {
      public:
        virtual ~WorkTaskPort() = default;

        [[nodiscard]] virtual std::optional<javelin::jmap::cache::DatabaseError>
        pause(std::string_view jobId) = 0;
        [[nodiscard]] virtual std::optional<javelin::jmap::cache::DatabaseError>
        resume(std::string_view jobId) = 0;
        [[nodiscard]] virtual std::optional<javelin::jmap::cache::DatabaseError>
        retry(std::string_view jobId) = 0;
        [[nodiscard]] virtual std::variant<std::vector<WorkRecord>,
                                           javelin::jmap::cache::DatabaseError>
        list() const = 0;
        [[nodiscard]] virtual QString summary() const = 0;
        [[nodiscard]] virtual QMetaObject::Connection
        connectChanged(QObject* context, std::function<void()> callback) = 0;
    };
} // namespace javelin::app
