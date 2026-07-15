#pragma once

#include "gui/search/SearchSession.h"

class QSettings;

namespace javelin::gui::search
{

    struct PersistedSearchState
    {
        javelin::jmap::search::EmailSearchCriteria criteria;
        RestoredSearchState restored;
    };

    [[nodiscard]] PersistedSearchState readSearchSessionSettings(const QSettings& settings);
    void writeSearchSessionSettings(QSettings& settings, const SearchSession& session);

} // namespace javelin::gui::search
