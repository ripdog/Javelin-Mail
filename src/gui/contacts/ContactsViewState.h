#pragma once

#include <QString>

#include <string>
#include <vector>

namespace javelin::gui::contacts
{
    struct ContactsViewState
    {
        std::string accountId;
        std::string addressBookId;
        std::string contactId;
        QString filter;
        int sortMode = 0;
        int groupFilterMode = 0;
        std::string groupId;
        std::vector<std::string> selectedContactKeys;
    };
} // namespace javelin::gui::contacts
