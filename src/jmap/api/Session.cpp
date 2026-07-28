#include "jmap/api/Session.h"

namespace javelin::jmap::api
{

    bool CapabilityValidationResult::ok() const
    {
        return errors.empty();
    }

    namespace
    {

        void appendIfMissing(std::vector<CapabilityError>& errors, const bool condition,
                             const CapabilityError error)
        {
            if (!condition)
            {
                errors.push_back(error);
            }
        }

    } // namespace

    CapabilityValidationResult validateSessionCapabilities(const Session& session,
                                                           const RequiredCapabilities& required)
    {
        CapabilityValidationResult result;
        appendIfMissing(result.errors, session.capabilities.core,
                        CapabilityError::MissingCoreCapability);
        if (session.capabilities.core)
        {
            appendIfMissing(result.errors, coreRequestLimits(session).has_value(),
                            CapabilityError::InvalidCoreCapability);
        }

        if (required.mail)
        {
            appendIfMissing(result.errors, session.capabilities.mail,
                            CapabilityError::MissingMailCapability);

            if (session.capabilities.mail)
            {
                const auto accountIt =
                    session.primaryAccounts.mailAccountId.has_value()
                        ? session.accounts.find(*session.primaryAccounts.mailAccountId)
                        : session.accounts.end();
                appendIfMissing(result.errors, accountIt != session.accounts.end(),
                                CapabilityError::MissingPrimaryMailAccount);

                if (accountIt != session.accounts.end())
                {
                    appendIfMissing(result.errors, accountIt->second.accountCapabilities.mail,
                                    CapabilityError::MissingMailAccountCapability);
                }
            }
        }

        if (required.submission)
        {
            appendIfMissing(result.errors, session.capabilities.submission,
                            CapabilityError::MissingSubmissionCapability);

            if (session.capabilities.submission)
            {
                const auto accountIt =
                    session.primaryAccounts.submissionAccountId.has_value()
                        ? session.accounts.find(*session.primaryAccounts.submissionAccountId)
                        : session.accounts.end();
                appendIfMissing(result.errors, accountIt != session.accounts.end(),
                                CapabilityError::MissingPrimarySubmissionAccount);

                if (accountIt != session.accounts.end())
                {
                    appendIfMissing(result.errors, accountIt->second.accountCapabilities.submission,
                                    CapabilityError::MissingSubmissionAccountCapability);
                }
            }
        }

        if (required.calendars)
        {
            appendIfMissing(result.errors, session.capabilities.calendars,
                            CapabilityError::MissingCalendarsCapability);

            if (session.capabilities.calendars)
            {
                const auto accountIt =
                    session.primaryAccounts.calendarsAccountId.has_value()
                        ? session.accounts.find(*session.primaryAccounts.calendarsAccountId)
                        : session.accounts.end();
                appendIfMissing(result.errors, accountIt != session.accounts.end(),
                                CapabilityError::MissingPrimaryCalendarsAccount);
                if (accountIt != session.accounts.end())
                {
                    appendIfMissing(result.errors,
                                    accountIt->second.accountCapabilities.calendars.has_value(),
                                    CapabilityError::MissingCalendarsAccountCapability);
                }
            }
        }

        if (required.sieve)
        {
            appendIfMissing(result.errors, session.capabilities.sieve,
                            CapabilityError::MissingSieveCapability);
            if (session.capabilities.sieve)
            {
                const auto accountIt =
                    session.primaryAccounts.sieveAccountId.has_value()
                        ? session.accounts.find(*session.primaryAccounts.sieveAccountId)
                        : session.accounts.end();
                appendIfMissing(result.errors, accountIt != session.accounts.end(),
                                CapabilityError::MissingPrimarySieveAccount);
                if (accountIt != session.accounts.end())
                {
                    appendIfMissing(result.errors, accountIt->second.accountCapabilities.sieve,
                                    CapabilityError::MissingSieveAccountCapability);
                }
            }
        }

        return result;
    }

    std::optional<CoreRequestLimits> coreRequestLimits(const Session& session)
    {
        const auto& core = session.capabilities.coreDetails;
        if (!core || !core->maxSizeRequest || !core->maxConcurrentRequests ||
            !core->maxCallsInRequest || !core->maxObjectsInGet || !core->maxObjectsInSet ||
            *core->maxSizeRequest == 0 || *core->maxConcurrentRequests == 0 ||
            *core->maxCallsInRequest == 0 || *core->maxObjectsInGet == 0 ||
            *core->maxObjectsInSet == 0)
        {
            return std::nullopt;
        }
        return CoreRequestLimits{
            .maxSizeRequest = *core->maxSizeRequest,
            .maxConcurrentRequests = *core->maxConcurrentRequests,
            .maxCallsInRequest = *core->maxCallsInRequest,
            .maxObjectsInGet = *core->maxObjectsInGet,
            .maxObjectsInSet = *core->maxObjectsInSet,
        };
    }

    std::string_view toString(const CapabilityError error)
    {
        switch (error)
        {
        case CapabilityError::MissingCoreCapability:
            return "missing_core_capability";
        case CapabilityError::InvalidCoreCapability:
            return "invalid_core_capability";
        case CapabilityError::MissingMailCapability:
            return "missing_mail_capability";
        case CapabilityError::MissingSubmissionCapability:
            return "missing_submission_capability";
        case CapabilityError::MissingPrimaryMailAccount:
            return "missing_primary_mail_account";
        case CapabilityError::MissingPrimarySubmissionAccount:
            return "missing_primary_submission_account";
        case CapabilityError::MissingMailAccountCapability:
            return "missing_mail_account_capability";
        case CapabilityError::MissingSubmissionAccountCapability:
            return "missing_submission_account_capability";
        case CapabilityError::MissingCalendarsCapability:
            return "missing_calendars_capability";
        case CapabilityError::MissingPrimaryCalendarsAccount:
            return "missing_primary_calendars_account";
        case CapabilityError::MissingCalendarsAccountCapability:
            return "missing_calendars_account_capability";
        case CapabilityError::MissingSieveCapability:
            return "missing_sieve_capability";
        case CapabilityError::MissingPrimarySieveAccount:
            return "missing_primary_sieve_account";
        case CapabilityError::MissingSieveAccountCapability:
            return "missing_sieve_account_capability";
        }

        return "unknown_capability_error";
    }

} // namespace javelin::jmap::api
