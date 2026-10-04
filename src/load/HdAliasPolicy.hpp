#pragma once

#include <algorithm>
#include <string>
#include <string_view>

namespace wxl_modern_m2
{
    // Only remove the optional archive namespace from an exact model family.
    // Texture requests and unrelated directories must retain their own routing.
    inline std::string CanonicalHdAlias(std::string_view requested, std::string_view served)
    {
        auto fold = [](std::string_view value) {
            std::string result(value);
            for (char& c : result) {
                if (c == '/') c = '\\';
                if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            }
            return result;
        };
        auto asked = fold(requested);
        auto canonical = fold(served);
        if (!asked.starts_with("hd\\") || canonical.starts_with("hd\\")) return {};
        asked.erase(0, 3);
        const auto dot = canonical.rfind('.');
        if (dot == std::string::npos || canonical.substr(dot) != ".m2") return {};
        canonical.resize(dot);
        if (!asked.starts_with(canonical)) return {};
        const auto tail = asked.substr(canonical.size());
        const auto extension = tail.rfind('.');
        if (extension == std::string::npos) return {};
        const auto suffix = tail.substr(extension);
        if (suffix != ".m2" && suffix != ".mdx" && suffix != ".skin" &&
            suffix != ".anim" && suffix != ".skel") return {};
        if (!std::all_of(tail.begin(), tail.begin() + extension,
            [](char c) { return (c >= '0' && c <= '9') || c == '-'; })) return {};
        return canonical + (suffix == ".mdx" ? ".m2" : tail);
    }
}
