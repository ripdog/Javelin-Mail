#include "jmap/JmapCore.h"

#include <QStringLiteral>

namespace javelin::jmap
{

    QString JmapCore::statusSummary() const
    {
        return QStringLiteral(
            "JMAP core scaffolded. Session discovery and typed protocol live here next.");
    }

} // namespace javelin::jmap
