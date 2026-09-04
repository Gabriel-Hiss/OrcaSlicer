#include "JvmOffStub.hpp"
#include "slic3r/plugin/package/PackageReader.hpp"
#include "slic3r/plugin/package/PluginMetadata.hpp"
namespace Slic3r { namespace Plugin { namespace Jvm {
bool inspect_jar_metadata(const std::string &jar_path, std::string &out_id, std::string &out_entry_class, std::string &out_error) {
    Slic3r::Plugin::Package::PluginMetadata meta;
    std::string err;
    if (!Slic3r::Plugin::Package::read_jar_metadata(jar_path, meta, err)) { out_error = err; return false; }
    out_id = meta.id;
    out_entry_class = meta.entry_class ? *meta.entry_class : std::string();
    if (out_id.empty()) { out_error = "plugin.json missing id"; return false; }
    return true;
}
}}} // namespace Slic3r::Plugin::Jvm
