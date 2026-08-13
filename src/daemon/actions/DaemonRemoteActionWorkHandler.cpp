#include "daemon/actions/DaemonRemoteActionDispatcher.h"

#include "app/WorkScheduler.h"
#include "daemon/DaemonServices.h"

namespace javelin::app
{
    javelin::protocol::CommandReply DaemonRemoteActionDispatcher::dispatchWorkAction(
        const javelin::protocol::CommandId& id,
        const javelin::protocol::RemoteActionCommand& command)
    {
        namespace actions = javelin::protocol::actions;
        switch (command.action.value)
        {
        case actions::WorkPause::id.value:
            return dispatchDecoded<actions::WorkPause>(
                id, command,
                [this, &id](const std::string& jobId)
                {
                    return acceptValue<actions::WorkPause>(id,
                                                           m_services.workScheduler().pause(jobId));
                });
        case actions::WorkResume::id.value:
            return dispatchDecoded<actions::WorkResume>(
                id, command,
                [this, &id](const std::string& jobId)
                {
                    return acceptValue<actions::WorkResume>(
                        id, m_services.workScheduler().resume(jobId));
                });
        case actions::WorkRetry::id.value:
            return dispatchDecoded<actions::WorkRetry>(
                id, command,
                [this, &id](const std::string& jobId)
                {
                    return acceptValue<actions::WorkRetry>(id,
                                                           m_services.workScheduler().retry(jobId));
                });
        case actions::WorkList::id.value:
            return acceptValue<actions::WorkList>(id, m_services.workScheduler().list());
        case actions::WorkSummary::id.value:
            return acceptValue<actions::WorkSummary>(id, m_services.workScheduler().summary());
        default:
            return reject(id, QStringLiteral("The work action is unsupported."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);
        }
    }
} // namespace javelin::app
