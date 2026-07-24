#pragma once

#include "app/MessageSelection.h"

#include <QModelIndex>

namespace javelin::gui::messages
{

    [[nodiscard]] javelin::app::MessageSelection
    messageSelectionForAction(QModelIndexList selectedRows, const QModelIndex& currentIndex,
                              bool excludeUnread = false);

} // namespace javelin::gui::messages
