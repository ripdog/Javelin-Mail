#include "gui/contacts/ContactGroupController.h"

#include <QCoroTask>

#include <utility>
#include <variant>

namespace javelin::gui::contacts
{
    ContactGroupController::ContactGroupController(
        javelin::app::ContactCommandPort& commandPort, QObject& context,
        std::function<std::optional<std::string>(std::string_view)> ownerAccountId,
        std::function<void(bool)> setBusy, std::function<void(QString, int)> statusMessage)
        : m_commandPort(commandPort), m_context(context),
          m_ownerAccountId(std::move(ownerAccountId)), m_setBusy(std::move(setBusy)),
          m_statusMessage(std::move(statusMessage))
    {
    }

    void ContactGroupController::setMembership(std::string accountId, std::string groupId,
                                               std::vector<std::string> memberUids,
                                               const bool included)
    {
        if (groupId.empty() || memberUids.empty())
            return;
        const auto owner = m_ownerAccountId(accountId).value_or(std::string{});
        m_setBusy(true);
        auto task =
            m_commandPort.setContactGroupMembership(owner, {.accountId = std::move(accountId),
                                                            .groupId = std::move(groupId),
                                                            .memberUids = std::move(memberUids),
                                                            .included = included});
        QCoro::connect(std::move(task), &m_context,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           m_setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               m_statusMessage(error->message, 10000);
                       });
    }

    void ContactGroupController::deleteGroup(std::string accountId, std::string groupId)
    {
        if (groupId.empty())
            return;
        const auto owner = m_ownerAccountId(accountId).value_or(std::string{});
        m_setBusy(true);
        auto task = m_commandPort.deleteContactGroup(
            owner, {.accountId = std::move(accountId), .groupId = std::move(groupId)});
        QCoro::connect(std::move(task), &m_context,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           m_setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               m_statusMessage(error->message, 10000);
                       });
    }
} // namespace javelin::gui::contacts
