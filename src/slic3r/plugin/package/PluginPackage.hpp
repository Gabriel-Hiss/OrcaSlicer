#pragma once

// Inspection never executes plugin code; readers work on raw file bytes.

#include "PluginMetadata.hpp"
#include "PackageReader.hpp"
#include "Hash.hpp"
#include "InstallState.hpp"

namespace Slic3r::Plugin::Package {
using ::Slic3r::Plugin::Package::PLUGIN_METADATA_SCHEMA_VERSION;
using ::Slic3r::Plugin::Package::PLUGIN_HOOK_ABI_VERSION;
using ::Slic3r::Plugin::Package::PLUGIN_PE_RESOURCE_TYPE;
using ::Slic3r::Plugin::Package::PLUGIN_PE_RESOURCE_ID;
using ::Slic3r::Plugin::Package::PLUGIN_JAR_ENTRY;
using ::Slic3r::Plugin::Package::PLUGIN_ELF_NOTE_SECTION;
using ::Slic3r::Plugin::Package::INSTALL_STATE_FILENAME;
} // namespace Slic3r::Plugin::Package
