#pragma once

namespace javelin::gui::compose
{
    struct TrackedSignatureRange
    {
        int start = -1;
        int end = -1;
    };

    [[nodiscard]] bool changeTouchesTrackedSignature(TrackedSignatureRange range, int position,
                                                     int removedCharacters, int addedCharacters);

    [[nodiscard]] bool shouldReplaceTrackedSignature(bool tracked, bool custom,
                                                     bool explicitlyRemoved,
                                                     bool forceInsert = false);
} // namespace javelin::gui::compose
