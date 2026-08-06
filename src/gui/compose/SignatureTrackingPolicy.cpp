#include "gui/compose/SignatureTrackingPolicy.h"

#include <algorithm>

namespace javelin::gui::compose
{
    bool changeTouchesTrackedSignature(const TrackedSignatureRange range, const int position,
                                       const int removedCharacters, const int addedCharacters)
    {
        if (range.start < 0 || range.end < range.start || position < 0 || removedCharacters < 0 ||
            addedCharacters < 0)
            return false;

        const bool removalOverlaps = removedCharacters > 0 && position < range.end &&
                                     position + removedCharacters > range.start;
        const bool insertionInside =
            addedCharacters > 0 && position >= range.start && position < range.end;
        return removalOverlaps || insertionInside;
    }

    bool shouldReplaceTrackedSignature(const bool tracked, const bool custom,
                                       const bool explicitlyRemoved, const bool forceInsert)
    {
        if (forceInsert)
            return true;
        return tracked && !custom && !explicitlyRemoved;
    }
} // namespace javelin::gui::compose
