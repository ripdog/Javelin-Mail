#pragma once

#include "protocol/ProtocolTypes.h"
#include "protocol/SettingsContract.h"

#include <QMetaObject>

#include <functional>
#include <optional>

class QObject;

namespace javelin::app
{
    class SettingsPort
    {
      public:
        virtual ~SettingsPort() = default;

        [[nodiscard]] virtual const javelin::protocol::SettingsSnapshot& settings() const = 0;
        [[nodiscard]] virtual std::optional<javelin::protocol::BoundaryError>
        updateSettings(javelin::protocol::SettingsRevision baseRevision,
                       javelin::protocol::SettingsUpdate update) = 0;
        [[nodiscard]] virtual QMetaObject::Connection
        connectSettingsChanged(QObject* context, std::function<void()> callback) = 0;
    };
} // namespace javelin::app
