#pragma once

#include <QCoroTask>

#include <QUrl>

#include <optional>
#include <string>

namespace javelin::jmap::api
{
    [[nodiscard]] QCoro::Task<std::optional<QUrl>> discoverSessionUrl(std::string configuredServer,
                                                                      std::string loginEmail);
}
