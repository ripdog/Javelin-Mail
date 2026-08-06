#include "gui/compose/SignatureTrackingPolicy.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("typing before a tracked signature preserves native tracking", "[gui][signature]")
{
    const javelin::gui::compose::TrackedSignatureRange signature{.start = 20, .end = 35};
    CHECK_FALSE(javelin::gui::compose::changeTouchesTrackedSignature(signature, 0, 0, 40));
    CHECK_FALSE(javelin::gui::compose::changeTouchesTrackedSignature(signature, 19, 0, 1));
}

TEST_CASE("edits inside a tracked signature mark it custom", "[gui][signature]")
{
    const javelin::gui::compose::TrackedSignatureRange signature{.start = 20, .end = 35};
    CHECK(javelin::gui::compose::changeTouchesTrackedSignature(signature, 20, 0, 1));
    CHECK(javelin::gui::compose::changeTouchesTrackedSignature(signature, 27, 2, 0));
    CHECK(javelin::gui::compose::changeTouchesTrackedSignature(signature, 34, 4, 0));
}

TEST_CASE("changes after a tracked signature do not mark it custom", "[gui][signature]")
{
    const javelin::gui::compose::TrackedSignatureRange signature{.start = 20, .end = 35};
    CHECK_FALSE(javelin::gui::compose::changeTouchesTrackedSignature(signature, 36, 0, 8));
    CHECK_FALSE(javelin::gui::compose::changeTouchesTrackedSignature(signature, 35, 0, 1));
    CHECK_FALSE(javelin::gui::compose::changeTouchesTrackedSignature(signature, 35, 0, 0));
}

TEST_CASE("identity changes replace only an unmodified tracked signature", "[gui][signature]")
{
    using javelin::gui::compose::shouldReplaceTrackedSignature;
    CHECK(shouldReplaceTrackedSignature(true, false, false));
    CHECK_FALSE(shouldReplaceTrackedSignature(true, true, false));
    CHECK_FALSE(shouldReplaceTrackedSignature(false, false, true));
    CHECK_FALSE(shouldReplaceTrackedSignature(false, false, false));
    CHECK(shouldReplaceTrackedSignature(false, true, true, true));
}
