#pragma once

#include "jmap/submission/ComposeTypes.h"

#include <QString>

#include <optional>

namespace javelin::jmap::submission
{

    [[nodiscard]] QString serializeDraftSnapshot(const DraftSnapshot& snapshot);
    [[nodiscard]] std::optional<DraftSnapshot> deserializeDraftSnapshot(const QString& json);

} // namespace javelin::jmap::submission
