#include "jmap/api/PatchObject.h"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <vector>

namespace javelin::jmap::api
{

    namespace
    {

        [[nodiscard]] std::string escapeSegment(const std::string_view segment)
        {
            std::string escaped;
            escaped.reserve(segment.size());
            for (const char character : segment)
            {
                if (character == '~')
                {
                    escaped += "~0";
                }
                else if (character == '/')
                {
                    escaped += "~1";
                }
                else
                {
                    escaped.push_back(character);
                }
            }
            return escaped;
        }

        [[nodiscard]] std::variant<std::string, PatchObjectError>
        unescapeSegment(const std::string_view segment, const std::string_view path)
        {
            std::string unescaped;
            unescaped.reserve(segment.size());
            for (std::size_t index = 0; index < segment.size(); ++index)
            {
                if (segment[index] != '~')
                {
                    unescaped.push_back(segment[index]);
                    continue;
                }
                if (index + 1 >= segment.size() ||
                    (segment[index + 1] != '0' && segment[index + 1] != '1'))
                {
                    return PatchObjectError{
                        .code = PatchObjectErrorCode::InvalidPointer,
                        .path = std::string{path},
                    };
                }
                unescaped.push_back(segment[index + 1] == '0' ? '~' : '/');
                ++index;
            }
            return unescaped;
        }

        [[nodiscard]] std::variant<std::vector<std::string>, PatchObjectError>
        pointerSegments(const std::string_view path)
        {
            std::vector<std::string> segments;
            std::size_t start = 0;
            while (true)
            {
                const auto slash = path.find('/', start);
                const auto encoded = path.substr(
                    start, slash == std::string_view::npos ? path.size() - start : slash - start);
                auto segment = unescapeSegment(encoded, path);
                if (const auto* error = std::get_if<PatchObjectError>(&segment))
                {
                    return *error;
                }
                segments.push_back(std::get<std::string>(std::move(segment)));
                if (slash == std::string_view::npos)
                {
                    break;
                }
                start = slash + 1;
            }
            return segments;
        }

        [[nodiscard]] bool isPrefix(const std::vector<std::string>& prefix,
                                    const std::vector<std::string>& path)
        {
            return prefix.size() < path.size() &&
                   std::equal(prefix.cbegin(), prefix.cend(), path.cbegin());
        }

        [[nodiscard]] std::string encodedPath(const std::vector<std::string>& segments)
        {
            std::string path;
            for (const auto& segment : segments)
            {
                if (!path.empty())
                    path.push_back('/');
                path += escapeSegment(segment);
            }
            return path;
        }

        [[nodiscard]] bool equalJson(const glz::generic_sorted& left,
                                     const glz::generic_sorted& right)
        {
            std::string leftJson;
            std::string rightJson;
            return !glz::write_json(left, leftJson) && !glz::write_json(right, rightJson) &&
                   leftJson == rightJson;
        }

        void appendPatch(const glz::generic_sorted::object_t& current,
                         const glz::generic_sorted::object_t& desired,
                         std::vector<std::string>& path, glz::generic_sorted& patch)
        {
            for (const auto& [key, currentValue] : current)
            {
                path.push_back(key);
                const auto desiredValue = desired.find(key);
                if (desiredValue == desired.end())
                {
                    patch[encodedPath(path)] = nullptr;
                }
                else if (currentValue.is_object() && desiredValue->second.is_object())
                {
                    appendPatch(currentValue.get_object(), desiredValue->second.get_object(), path,
                                patch);
                }
                else if (!equalJson(currentValue, desiredValue->second))
                {
                    patch[encodedPath(path)] = desiredValue->second;
                }
                path.pop_back();
            }
            for (const auto& [key, desiredValue] : desired)
            {
                if (current.contains(key))
                    continue;
                path.push_back(key);
                patch[encodedPath(path)] = desiredValue;
                path.pop_back();
            }
        }

    } // namespace

    std::string patchPath(const std::span<const std::string_view> segments)
    {
        std::string path;
        for (const auto segment : segments)
        {
            if (!path.empty())
            {
                path.push_back('/');
            }
            path += escapeSegment(segment);
        }
        return path;
    }

    std::string patchPath(const std::string_view property, const std::string_view mapKey)
    {
        const std::array segments{property, mapKey};
        return patchPath(segments);
    }

    std::variant<std::string, PatchObjectError> makePatchObject(const std::string_view objectJson,
                                                                const std::string_view desiredJson)
    {
        glz::generic_sorted object;
        glz::generic_sorted desired;
        if (glz::read_json(object, objectJson) || glz::read_json(desired, desiredJson))
            return PatchObjectError{.code = PatchObjectErrorCode::InvalidJson, .path = {}};
        const auto* objectValues = object.get_if<glz::generic_sorted::object_t>();
        const auto* desiredValues = desired.get_if<glz::generic_sorted::object_t>();
        if (objectValues == nullptr || desiredValues == nullptr)
            return PatchObjectError{.code = PatchObjectErrorCode::RootNotObject, .path = {}};

        glz::generic_sorted patch;
        patch.data = glz::generic_sorted::object_t{};
        std::vector<std::string> path;
        appendPatch(*objectValues, *desiredValues, path, patch);

        std::string result;
        if (glz::write_json(patch, result))
            return PatchObjectError{.code = PatchObjectErrorCode::InvalidJson, .path = {}};
        return result;
    }

    std::variant<std::string, PatchObjectError> applyPatchObject(const std::string_view objectJson,
                                                                 const std::string_view patchJson)
    {
        glz::generic object;
        glz::generic patch;
        if (glz::read_json(object, objectJson) || glz::read_json(patch, patchJson))
        {
            return PatchObjectError{.code = PatchObjectErrorCode::InvalidJson, .path = {}};
        }
        auto* objectValues = object.get_if<glz::generic::object_t>();
        const auto* patchValues = patch.get_if<glz::generic::object_t>();
        if (objectValues == nullptr || patchValues == nullptr)
        {
            return PatchObjectError{.code = PatchObjectErrorCode::RootNotObject, .path = {}};
        }

        std::vector<std::pair<std::string, std::vector<std::string>>> paths;
        paths.reserve(patchValues->size());
        for (const auto& [path, value] : *patchValues)
        {
            static_cast<void>(value);
            auto segments = pointerSegments(path);
            if (const auto* error = std::get_if<PatchObjectError>(&segments))
            {
                return *error;
            }
            paths.emplace_back(path, std::get<std::vector<std::string>>(std::move(segments)));
        }
        for (std::size_t left = 0; left < paths.size(); ++left)
        {
            for (std::size_t right = left + 1; right < paths.size(); ++right)
            {
                if (isPrefix(paths[left].second, paths[right].second) ||
                    isPrefix(paths[right].second, paths[left].second))
                {
                    return PatchObjectError{
                        .code = PatchObjectErrorCode::ConflictingPointers,
                        .path = paths[left].first,
                    };
                }
            }
        }

        for (const auto& [path, segments] : paths)
        {
            auto* parent = objectValues;
            for (std::size_t index = 0; index + 1 < segments.size(); ++index)
            {
                const auto child = parent->find(segments[index]);
                if (child == parent->end())
                {
                    return PatchObjectError{
                        .code = PatchObjectErrorCode::MissingParent,
                        .path = path,
                    };
                }
                if (child->second.is_array())
                {
                    return PatchObjectError{
                        .code = PatchObjectErrorCode::ArrayTraversal,
                        .path = path,
                    };
                }
                parent = child->second.get_if<glz::generic::object_t>();
                if (parent == nullptr)
                {
                    return PatchObjectError{
                        .code = PatchObjectErrorCode::MissingParent,
                        .path = path,
                    };
                }
            }

            const auto& value = patchValues->at(path);
            const auto& property = segments.back();
            if (value.holds<glz::generic::null_t>())
            {
                parent->erase(property);
            }
            else
            {
                parent->insert_or_assign(property, value);
            }
        }

        std::string result;
        if (glz::write_json(object, result))
        {
            return PatchObjectError{.code = PatchObjectErrorCode::InvalidJson, .path = {}};
        }
        return result;
    }

} // namespace javelin::jmap::api
