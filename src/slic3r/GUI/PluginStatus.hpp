#pragma once

#include <string>

namespace Slic3r
{
    namespace GUI
    {
        enum class PluginStatus
        {
            // IMPORTANT: ordinal order is the Plugins dialog Status sort priority.
            Activated,
            Error,
            Incompatible,
            Inactive,
            Loading,
            RestartRequired
        };

        inline std::string to_string(PluginStatus status)
        {
            switch (status)
            {
            case PluginStatus::Activated: return "Activated";
            case PluginStatus::Error: return "Error";
            case PluginStatus::Incompatible: return "Incompatible";
            case PluginStatus::Inactive: return "Inactive";
            case PluginStatus::Loading: return "Loading";
            case PluginStatus::RestartRequired: return "RestartRequired";
            }

            return "Inactive";
        }

        inline PluginStatus plugin_status_from_string(const std::string &s, PluginStatus fallback = PluginStatus::Inactive)
        {
            if (s == "Activated") return PluginStatus::Activated;
            if (s == "Error") return PluginStatus::Error;
            if (s == "Incompatible") return PluginStatus::Incompatible;
            if (s == "Inactive") return PluginStatus::Inactive;
            if (s == "Loading") return PluginStatus::Loading;
            if (s == "RestartRequired") return PluginStatus::RestartRequired;
            return fallback;
        }
    }
} // namespace Slic3r::GUI
