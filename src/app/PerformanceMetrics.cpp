#include "app/PerformanceMetrics.h"

#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStringList>

#include <algorithm>
#include <chrono>

#if defined(Q_OS_UNIX)
#include <sys/resource.h>
#endif

Q_LOGGING_CATEGORY(logPerformance, "javelin.performance")

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] QString metricToken(QString value)
        {
            value.replace(QLatin1String(" "), QStringLiteral("_"));
            value.replace(QLatin1String("\t"), QStringLiteral("_"));
            value.replace(QLatin1String("\r"), QStringLiteral("_"));
            value.replace(QLatin1String("\n"), QStringLiteral("_"));
            value.replace(QLatin1String("="), QStringLiteral("_"));
            return value;
        }

        [[nodiscard]] QString quotedMetricValue(QString value)
        {
            value.replace(QLatin1String("\\"), QStringLiteral("\\\\"));
            value.replace(QLatin1String("\""), QStringLiteral("\\\""));
            value.replace(QLatin1String("\r"), QStringLiteral("\\r"));
            value.replace(QLatin1String("\n"), QStringLiteral("\\n"));
            return QStringLiteral("\"") + value + QStringLiteral("\"");
        }

        [[nodiscard]] std::optional<qint64> currentResidentSetKiB()
        {
            QFile statusFile{QStringLiteral("/proc/self/status")};
            if (!statusFile.open(QIODevice::ReadOnly))
                return std::nullopt;

            while (!statusFile.atEnd())
            {
                const auto line = statusFile.readLine().simplified();
                if (!line.startsWith(QByteArrayLiteral("VmRSS:")))
                    continue;
                const auto fields = line.mid(line.indexOf(':') + 1).trimmed().split(' ');
                if (fields.isEmpty())
                    return std::nullopt;
                bool ok = false;
                const auto value = fields.front().toLongLong(&ok);
                return ok ? std::optional<qint64>{value} : std::nullopt;
            }
            return std::nullopt;
        }

#if defined(Q_OS_UNIX)
        [[nodiscard]] qint64 timeValueMicroseconds(const timeval& value)
        {
            return static_cast<qint64>(value.tv_sec) * 1'000'000 +
                   static_cast<qint64>(value.tv_usec);
        }
#endif
    } // namespace

    bool PerformanceMetrics::enabled()
    {
        static const bool value = []
        {
            bool ok = false;
            const int configured = qEnvironmentVariableIntValue("JAVELIN_UI_PROFILING", &ok);
            return ok && configured > 0;
        }();
        return value;
    }

    void PerformanceMetrics::recordDuration(QString process, QString operation,
                                            const std::chrono::microseconds duration,
                                            QString outcome, QString details)
    {
        if (!enabled())
            return;
        qCInfo(logPerformance).noquote()
            << formatMetric(std::move(process), std::move(operation), duration, std::move(outcome),
                            std::move(details));
    }

    void PerformanceMetrics::recordEvent(QString process, QString operation, QString outcome,
                                         QString details)
    {
        if (!enabled())
            return;
        qCInfo(logPerformance).noquote()
            << formatMetric(std::move(process), std::move(operation), std::nullopt,
                            std::move(outcome), std::move(details));
    }

    void PerformanceMetrics::recordProcessResources(QString process, QString databasePath)
    {
        if (!enabled())
            return;

        QStringList details;
        if (const auto residentSet = currentResidentSetKiB(); residentSet.has_value())
            details.push_back(QStringLiteral("rss_kib=%1").arg(*residentSet));

        const auto maximumResidentSet = [&]() -> std::optional<qint64>
        {
#if defined(Q_OS_UNIX)
            struct rusage usage{};
            if (getrusage(RUSAGE_SELF, &usage) != 0)
                return std::nullopt;
#if defined(Q_OS_MACOS)
            return static_cast<qint64>(usage.ru_maxrss) / 1024;
#else
            return static_cast<qint64>(usage.ru_maxrss);
#endif
#else
            return std::nullopt;
#endif
        }();
        if (maximumResidentSet.has_value())
            details.push_back(QStringLiteral("max_rss_kib=%1").arg(*maximumResidentSet));

#if defined(Q_OS_UNIX)
        struct rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) == 0)
        {
            details.push_back(
                QStringLiteral("user_cpu_us=%1").arg(timeValueMicroseconds(usage.ru_utime)));
            details.push_back(
                QStringLiteral("system_cpu_us=%1").arg(timeValueMicroseconds(usage.ru_stime)));
        }
#endif

        if (!databasePath.isEmpty())
        {
            const auto walPath = databasePath + QStringLiteral("-wal");
            const auto walBytes = QFileInfo{walPath}.exists() ? QFileInfo{walPath}.size() : 0;
            details.push_back(QStringLiteral("wal_bytes=%1").arg(walBytes));
        }
        if (details.isEmpty())
            details.push_back(QStringLiteral("sample_unavailable=true"));

        recordEvent(std::move(process), QStringLiteral("process_resources"),
                    QStringLiteral("sample"), details.join(QLatin1Char(' ')));
    }

    QString
    PerformanceMetrics::formatMetric(QString process, QString operation,
                                     const std::optional<std::chrono::microseconds> duration,
                                     QString outcome, QString details)
    {
        QString result =
            QStringLiteral("metric process=%1 operation=%2")
                .arg(metricToken(std::move(process)), metricToken(std::move(operation)));
        if (!outcome.isEmpty())
            result += QStringLiteral(" outcome=%1").arg(metricToken(std::move(outcome)));
        if (duration.has_value())
        {
            const auto microseconds = std::max<qint64>(0, duration->count());
            result += QStringLiteral(" duration_us=%1").arg(microseconds);
        }
        if (!details.isEmpty())
            result += QStringLiteral(" details=%1").arg(quotedMetricValue(std::move(details)));
        return result;
    }

    QString PerformanceMetrics::remoteActionName(const protocol::RemoteActionKind kind)
    {
        using Kind = protocol::RemoteActionKind;
        switch (kind)
        {
        case Kind::RemoveConfiguredAccount:
            return QStringLiteral("remove_configured_account");
        case Kind::CalendarReadCached:
            return QStringLiteral("calendar_read_cached");
        case Kind::CalendarReadAccounts:
            return QStringLiteral("calendar_read_accounts");
        case Kind::CalendarReadCalendars:
            return QStringLiteral("calendar_read_calendars");
        case Kind::CalendarRequestRange:
            return QStringLiteral("calendar_request_range");
        case Kind::CalendarCreateEvent:
            return QStringLiteral("calendar_create_event");
        case Kind::CalendarUpdateEvent:
            return QStringLiteral("calendar_update_event");
        case Kind::CalendarDeleteEvent:
            return QStringLiteral("calendar_delete_event");
        case Kind::CalendarSetSubscribed:
            return QStringLiteral("calendar_set_subscribed");
        case Kind::CalendarSetDefault:
            return QStringLiteral("calendar_set_default");
        case Kind::CalendarCreate:
            return QStringLiteral("calendar_create");
        case Kind::CalendarDelete:
            return QStringLiteral("calendar_delete");
        case Kind::CalendarSetVisible:
            return QStringLiteral("calendar_set_visible");
        case Kind::ComposeOpen:
            return QStringLiteral("compose_open");
        case Kind::ComposeLoadSenderIdentities:
            return QStringLiteral("compose_load_sender_identities");
        case Kind::ComposeSaveDraft:
            return QStringLiteral("compose_save_draft");
        case Kind::ComposeSend:
            return QStringLiteral("compose_send");
        case Kind::ComposeLoadWorkingCopy:
            return QStringLiteral("compose_load_working_copy");
        case Kind::ComposeStoreWorkingCopy:
            return QStringLiteral("compose_store_working_copy");
        case Kind::ComposeDiscard:
            return QStringLiteral("compose_discard");
        case Kind::ContactRequestRefresh:
            return QStringLiteral("contact_request_refresh");
        case Kind::ContactMutateAddressBook:
            return QStringLiteral("contact_mutate_address_book");
        case Kind::ContactSave:
            return QStringLiteral("contact_save");
        case Kind::ContactSetStarred:
            return QStringLiteral("contact_set_starred");
        case Kind::ContactDelete:
            return QStringLiteral("contact_delete");
        case Kind::ContactCreateGroup:
            return QStringLiteral("contact_create_group");
        case Kind::ContactDeleteGroup:
            return QStringLiteral("contact_delete_group");
        case Kind::ContactSetGroupMembership:
            return QStringLiteral("contact_set_group_membership");
        case Kind::ContactCopy:
            return QStringLiteral("contact_copy");
        case Kind::ContactImport:
            return QStringLiteral("contact_import");
        case Kind::ContactMerge:
            return QStringLiteral("contact_merge");
        case Kind::ContactUploadMedia:
            return QStringLiteral("contact_upload_media");
        case Kind::ContactDownloadMedia:
            return QStringLiteral("contact_download_media");
        case Kind::MailQueueMailboxMutation:
            return QStringLiteral("mail_queue_mailbox_mutation");
        case Kind::MailQueueDestroy:
            return QStringLiteral("mail_queue_destroy");
        case Kind::MailQueueMarkUnread:
            return QStringLiteral("mail_queue_mark_unread");
        case Kind::MailQueueMarkRead:
            return QStringLiteral("mail_queue_mark_read");
        case Kind::MailQueueSetFlagged:
            return QStringLiteral("mail_queue_set_flagged");
        case Kind::MailSubmitPending:
            return QStringLiteral("mail_submit_pending");
        case Kind::SieveList:
            return QStringLiteral("sieve_list");
        case Kind::SieveGet:
            return QStringLiteral("sieve_get");
        case Kind::SieveValidate:
            return QStringLiteral("sieve_validate");
        case Kind::SieveSave:
            return QStringLiteral("sieve_save");
        case Kind::SieveDelete:
            return QStringLiteral("sieve_delete");
        case Kind::SieveActivate:
            return QStringLiteral("sieve_activate");
        case Kind::AccountBootstrap:
            return QStringLiteral("account_bootstrap");
        case Kind::MessageContent:
            return QStringLiteral("message_content");
        case Kind::AttachmentDownload:
            return QStringLiteral("attachment_download");
        case Kind::MessageSource:
            return QStringLiteral("message_source");
        case Kind::MailboxObserve:
            return QStringLiteral("mailbox_observe");
        case Kind::MailboxUnobserve:
            return QStringLiteral("mailbox_unobserve");
        case Kind::MailboxWindow:
            return QStringLiteral("mailbox_window");
        case Kind::SearchWindow:
            return QStringLiteral("search_window");
        case Kind::SearchRetire:
            return QStringLiteral("search_retire");
        case Kind::Undo:
            return QStringLiteral("undo");
        case Kind::Redo:
            return QStringLiteral("redo");
        case Kind::UndoAcknowledgeRemove:
            return QStringLiteral("undo_acknowledge_remove");
        case Kind::UndoForget:
            return QStringLiteral("undo_forget");
        case Kind::UndoSnapshot:
            return QStringLiteral("undo_snapshot");
        case Kind::ReloadSettings:
            return QStringLiteral("reload_settings");
        case Kind::WorkPause:
            return QStringLiteral("work_pause");
        case Kind::WorkResume:
            return QStringLiteral("work_resume");
        case Kind::WorkRetry:
            return QStringLiteral("work_retry");
        case Kind::WorkList:
            return QStringLiteral("work_list");
        case Kind::WorkSummary:
            return QStringLiteral("work_summary");
        case Kind::OnboardingDiscover:
            return QStringLiteral("onboarding_discover");
        case Kind::OnboardingStartOAuth:
            return QStringLiteral("onboarding_start_oauth");
        case Kind::OnboardingFinishOAuth:
            return QStringLiteral("onboarding_finish_oauth");
        case Kind::OnboardingAuthenticateManually:
            return QStringLiteral("onboarding_authenticate_manually");
        case Kind::OnboardingRevokeOAuth:
            return QStringLiteral("onboarding_revoke_oauth");
        case Kind::OnboardingCancelOAuth:
            return QStringLiteral("onboarding_cancel_oauth");
        case Kind::DeveloperDiagnosticsSnapshot:
            return QStringLiteral("developer_diagnostics_snapshot");
        }
        return QStringLiteral("unknown");
    }

    PerformanceSpan::PerformanceSpan(QString process, QString operation, QString details)
        : m_process(std::move(process)), m_operation(std::move(operation)),
          m_details(std::move(details)), m_startedAt(std::chrono::steady_clock::now()),
          m_enabled(PerformanceMetrics::enabled())
    {
    }

    PerformanceSpan::~PerformanceSpan()
    {
        finish();
    }

    void PerformanceSpan::finish(QString outcome, QString details)
    {
        if (m_finished)
            return;
        m_finished = true;
        if (!m_enabled)
            return;

        if (!m_details.isEmpty() && !details.isEmpty())
            m_details += QLatin1Char(' ');
        m_details += std::move(details);
        PerformanceMetrics::recordDuration(std::move(m_process), std::move(m_operation),
                                           std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - m_startedAt),
                                           std::move(outcome), std::move(m_details));
    }
} // namespace javelin::app
