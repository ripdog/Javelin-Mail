#pragma once

#include <Qt>

namespace javelin::gui::contacts
{
    enum class GroupFilterMode
    {
        All = 0,
        Starred = 1,
        Group = 2,
        Divider = 3,
        Ungrouped = 4,
        AddressBook = 5,
        AccountStarred = 6,
        NoSubscribedAddressBooks = 7,
    };

    inline constexpr int groupFilterModeRole = Qt::UserRole + 10;
    inline constexpr int groupIdRole = Qt::UserRole + 11;
    inline constexpr int contactAccountIdRole = Qt::UserRole + 12;
    inline constexpr int contactUidRole = Qt::UserRole + 13;
    inline constexpr int stableItemKeyRole = Qt::UserRole + 14;
    inline constexpr int groupAccountIdRole = Qt::UserRole + 15;
    inline constexpr int groupAddressBookIdRole = Qt::UserRole + 16;
} // namespace javelin::gui::contacts
