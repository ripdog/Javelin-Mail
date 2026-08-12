#pragma once

#include "protocol/ProtocolTypes.h"
#include "protocol/SettingsContract.h"

#include <QMetaObject>

#include <functional>
#include <optional>

class QObject;

namespace javelin::gui::settings
{
    class WorkspaceSettingsPort
    {
      public:
        virtual ~WorkspaceSettingsPort() = default;

        [[nodiscard]] virtual const javelin::protocol::WorkspaceSettings&
        workspaceSettings() const = 0;
        [[nodiscard]] virtual std::optional<javelin::protocol::BoundaryError>
        updateWorkspace(javelin::protocol::WorkspaceSettings workspace) = 0;
        [[nodiscard]] virtual QMetaObject::Connection
        connectWorkspaceChanged(QObject* context, std::function<void()> callback) = 0;
    };
} // namespace javelin::gui::settings
