#pragma once

#include <QString>

namespace javelin::jmap
{

    class JmapCore
    {
      public:
        [[nodiscard]] QString statusSummary() const;
    };

} // namespace javelin::jmap
