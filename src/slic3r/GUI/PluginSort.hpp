#pragma once

#include "PluginStatus.hpp"

#include "libslic3r/Semver.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r::GUI
{
    enum class PluginSortKey
    {
        Status,
        Name,
        Version,
        None
    };

    enum class PluginSortOrder
    {
        Asc,
        Desc
    };

    inline std::string to_string(PluginSortKey sort_key)
    {
        switch (sort_key)
        {
        case PluginSortKey::Status: return "status";
        case PluginSortKey::Name: return "name";
        case PluginSortKey::Version: return "version";
        case PluginSortKey::None: return "none";
        }
        return "status";
    }

    inline std::string to_string(PluginSortOrder sort_order)
    {
        return sort_order == PluginSortOrder::Desc ? "desc" : "asc";
    }

    inline PluginSortKey plugin_sort_key_from_string(const std::string& sort_key, PluginSortKey fallback)
    {
        if (sort_key == "status") return PluginSortKey::Status;
        if (sort_key == "name") return PluginSortKey::Name;
        if (sort_key == "version") return PluginSortKey::Version;
        if (sort_key == "none") return PluginSortKey::None;
        return fallback;
    }

    inline PluginSortOrder plugin_sort_order_from_string(const std::string& sort_order, PluginSortOrder fallback)
    {
        if (sort_order == "asc") return PluginSortOrder::Asc;
        if (sort_order == "desc") return PluginSortOrder::Desc;
        return fallback;
    }

    inline int compare_ascii_case_insensitive_natural(const std::string& lhs, const std::string& rhs)
    {
        std::size_t li = 0;
        std::size_t ri = 0;
        while (li < lhs.size() && ri < rhs.size())
        {
            const unsigned char lc = static_cast<unsigned char>(lhs[li]);
            const unsigned char rc = static_cast<unsigned char>(rhs[ri]);
            if (std::isdigit(lc) && std::isdigit(rc))
            {
                const std::size_t lhs_digit_begin = li;
                const std::size_t rhs_digit_begin = ri;
                while (li < lhs.size() && std::isdigit(static_cast<unsigned char>(lhs[li]))) ++li;
                while (ri < rhs.size() && std::isdigit(static_cast<unsigned char>(rhs[ri]))) ++ri;
                const std::string_view lhs_run(lhs.data() + lhs_digit_begin, li - lhs_digit_begin);
                const std::string_view rhs_run(rhs.data() + rhs_digit_begin, ri - rhs_digit_begin);
                const std::string_view lhs_num = lhs_run.substr(std::min(lhs_run.find_first_not_of('0'), lhs_run.size()));
                const std::string_view rhs_num = rhs_run.substr(std::min(rhs_run.find_first_not_of('0'), rhs_run.size()));
                if (lhs_num.size() != rhs_num.size()) return lhs_num.size() < rhs_num.size() ? -1 : 1;
                if (const int cmp = lhs_num.compare(rhs_num); cmp != 0) return cmp;
                if (lhs_run.size() != rhs_run.size()) return lhs_run.size() < rhs_run.size() ? -1 : 1;
                continue;
            }
            const int lower_lhs = std::tolower(lc);
            const int lower_rhs = std::tolower(rc);
            if (lower_lhs != lower_rhs) return lower_lhs < lower_rhs ? -1 : 1;
            ++li; ++ri;
        }
        if (li == lhs.size() && ri == rhs.size()) return 0;
        return li == lhs.size() ? -1 : 1;
    }

    template <class PluginItem>
    int compare_plugin_base_order(const PluginItem& lhs, const PluginItem& rhs)
    {
        if (const int cmp = compare_ascii_case_insensitive_natural(lhs.display_name, rhs.display_name); cmp != 0) return cmp;
        if (const int cmp = static_cast<int>(lhs.status) - static_cast<int>(rhs.status); cmp != 0) return cmp;
        if (const int cmp = compare_ascii_case_insensitive_natural(lhs.version, rhs.version); cmp != 0) return cmp;
        return lhs.id.compare(rhs.id);
    }

    inline int compare_plugin_version(const std::string& lhs, const std::string& rhs)
    {
        const auto lhs_semver = Semver::parse(lhs);
        const auto rhs_semver = Semver::parse(rhs);
        if (lhs_semver && rhs_semver) {
            if (*lhs_semver < *rhs_semver) return -1;
            if (*rhs_semver < *lhs_semver) return 1;
            return 0;
        }
        return compare_ascii_case_insensitive_natural(lhs, rhs);
    }

    template <class PluginItem>
    int compare_plugin_sort_key(const PluginItem& lhs, const PluginItem& rhs, PluginSortKey sort_key)
    {
        switch (sort_key) {
        case PluginSortKey::Status: return static_cast<int>(lhs.status) - static_cast<int>(rhs.status);
        case PluginSortKey::Name: return compare_ascii_case_insensitive_natural(lhs.display_name, rhs.display_name);
        case PluginSortKey::Version: return compare_plugin_version(lhs.version, rhs.version);
        case PluginSortKey::None: return 0;
        }
        return 0;
    }

    template <class PluginItem>
    void sort_plugin_items_for_dialog(std::vector<PluginItem>& items, PluginSortKey sort_key, PluginSortOrder sort_order)
    {
        std::sort(items.begin(), items.end(), [sort_key, sort_order](const PluginItem& lhs, const PluginItem& rhs) {
            if (const int cmp = compare_plugin_sort_key(lhs, rhs, sort_key); cmp != 0)
                return sort_order == PluginSortOrder::Asc ? cmp < 0 : cmp > 0;
            return compare_plugin_base_order(lhs, rhs) < 0;
        });
    }
} // namespace Slic3r::GUI
