#pragma once

#include "app/SearchSession.h"

#include <QString>
#include <QVariantMap>

namespace javelin::gui::search
{

    struct PersistedSearchState
    {
        javelin::jmap::search::EmailSearchCriteria criteria;
        javelin::app::RestoredSearchState restored;
    };

    [[nodiscard]] PersistedSearchState readSearchSessionSettings(const QVariantMap& settings,
                                                                 const QString& prefix);
    void writeSearchSessionSettings(QVariantMap& settings, const QString& prefix,
                                    const PersistedSearchState& state);

} // namespace javelin::gui::search
