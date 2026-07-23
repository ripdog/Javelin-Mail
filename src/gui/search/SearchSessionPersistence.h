#pragma once

#include "app/SearchSession.h"

class QSettings;

namespace javelin::gui::search
{

    struct PersistedSearchState
    {
        javelin::jmap::search::EmailSearchCriteria criteria;
        javelin::app::RestoredSearchState restored;
    };

    [[nodiscard]] PersistedSearchState readSearchSessionSettings(const QSettings& settings);
    void writeSearchSessionSettings(QSettings& settings,
                                    const javelin::app::SearchSession& session);

} // namespace javelin::gui::search
