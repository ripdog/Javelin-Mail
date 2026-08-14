#pragma once

#include "protocol/actions/ActionCatalog.h"

#include <QString>

namespace javelin::protocol
{
    [[nodiscard]] inline QString actionDisplayName(const ActionId action)
    {
        return actions::actionName(action);
    }
} // namespace javelin::protocol
