#pragma once

#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::jmap::api
{

    enum class PatchObjectErrorCode
    {
        InvalidJson,
        RootNotObject,
        InvalidPointer,
        ConflictingPointers,
        MissingParent,
        ArrayTraversal,
    };

    struct PatchObjectError
    {
        PatchObjectErrorCode code = PatchObjectErrorCode::InvalidJson;
        std::string path;
    };

    [[nodiscard]] std::string patchPath(std::span<const std::string_view> segments);
    [[nodiscard]] std::string patchPath(std::string_view property, std::string_view mapKey);
    [[nodiscard]] std::variant<std::string, PatchObjectError>
    applyPatchObject(std::string_view objectJson, std::string_view patchJson);

} // namespace javelin::jmap::api
