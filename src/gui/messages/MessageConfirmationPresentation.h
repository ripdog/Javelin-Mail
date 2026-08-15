#pragma once

#include "app/MessageSelection.h"

#include <QModelIndex>
#include <QString>

namespace javelin::gui::messages
{
    struct PermanentDeleteConfirmation
    {
        QString prompt;
        QString details;
    };

    [[nodiscard]] PermanentDeleteConfirmation
    permanentDeleteConfirmation(const javelin::app::MessageSelection& selection,
                                QModelIndexList selectedRows, const QModelIndex& currentIndex);
} // namespace javelin::gui::messages
