#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
    #include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Slic3r {
PrintRegionConfig region_config_from_model_volume(const PrintRegionConfig &default_or_parent_region_config,
                                                  const DynamicPrintConfig *layer_range_config,
                                                  const ModelVolume &volume,
                                                  size_t num_extruders);
}

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

using ModifierOption = std::pair<const char *, ConfigOption *>;

DynamicPrintConfig filament_modifier_test_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",                      0.2 },
        { "initial_layer_print_height",        0.2 },
        { "initial_layer_line_width",          0 },
        { "machine_start_gcode",               "" },
        { "machine_end_gcode",                 "" },
        { "gcode_comments",                    true },
        { "z_hop",                             0 },
        { "skirts",                            0 },
        { "brim_type",                         "no_brim" },
        { "slow_down_for_layer_cooling",       false },
        { "nozzle_temperature",                "220" },
        { "nozzle_temperature_initial_layer",  "220" },
        { "nozzle_temperature_range_low",      "190" },
        { "nozzle_temperature_range_high",     "240" },
        { "filament_max_volumetric_speed",     "20" },
        { "outer_wall_speed",                  "60" },
        { "inner_wall_speed",                  "80" },
        { "sparse_infill_speed",               "80" }
    });
    return config;
}

ModelVolume* add_cube_model_part(ModelObject &object)
{
    return object.add_volume(make_cube(20., 20., 20.), ModelVolumeType::MODEL_PART);
}

ModelVolume* add_filament_modifier_cube(ModelObject &object, double width = 20.)
{
    ModelVolume *modifier = object.add_volume(make_cube(width, 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
    modifier->set_filament_modifier(true);
    return modifier;
}

void prepare_print(Print &print, Model &model, const DynamicPrintConfig &config)
{
    arrange_objects(model, InfiniteBed{}, ArrangeParams{ scaled(min_object_distance(config)) });
    for (ModelObject *model_object : model.objects) {
        model_object->ensure_on_bed();
        print.auto_assign_extruders(model_object);
    }

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
}

std::string slice_cube_with_optional_filament_modifier(
    const DynamicPrintConfig &config,
    double modifier_width = 0.,
    std::initializer_list<ModifierOption> modifier_options = {})
{
    Model model;
    ModelObject *object = model.add_object();
    object->name = "filament_modifier_test.stl";
    add_cube_model_part(*object);

    if (modifier_width > 0.) {
        ModelVolume *modifier = add_filament_modifier_cube(*object, modifier_width);
        for (const ModifierOption &option : modifier_options)
            modifier->config.set_key_value(option.first, option.second);
    }

    object->add_instance();

    Print print;
    prepare_print(print, model, config);
    return Slic3r::Test::gcode(print);
}

ModelVolume* add_fixture_with_filament_modifiers(
    Model &model,
    TestMesh fixture,
    std::initializer_list<ModifierOption> modifier_options,
    std::initializer_list<ModifierOption> overlapping_modifier_options = {})
{
    TriangleMesh part = mesh(fixture);
    const BoundingBoxf3 bounds = part.bounding_box();
    const Vec3d size = bounds.size();

    TriangleMesh modifier_mesh = make_cube(size.x() * 0.5, size.y() + 20., size.z());
    modifier_mesh.translate(static_cast<float>(bounds.min.x()),
                            static_cast<float>(bounds.min.y() - 10.),
                            static_cast<float>(bounds.min.z()));

    ModelObject *object = model.add_object();
    object->name = "filament_modifier_fixture.stl";
    object->add_volume(std::move(part), ModelVolumeType::MODEL_PART);

    auto add_modifier = [object](TriangleMesh &&mesh, std::initializer_list<ModifierOption> options) {
        ModelVolume *modifier = object->add_volume(std::move(mesh), ModelVolumeType::PARAMETER_MODIFIER);
        modifier->set_filament_modifier(true);
        for (const ModifierOption &option : options)
            modifier->config.set_key_value(option.first, option.second);
        return modifier;
    };

    TriangleMesh overlapping_mesh;
    if (!overlapping_modifier_options.empty())
        overlapping_mesh = modifier_mesh;
    ModelVolume *modifier = add_modifier(std::move(modifier_mesh), modifier_options);
    if (!overlapping_modifier_options.empty())
        add_modifier(std::move(overlapping_mesh), overlapping_modifier_options);
    object->add_instance();
    return modifier;
}

std::string slice_fixture_with_filament_modifier(
    const DynamicPrintConfig &config,
    TestMesh fixture,
    std::initializer_list<ModifierOption> modifier_options,
    std::initializer_list<ModifierOption> overlapping_modifier_options = {})
{
    Model model;
    add_fixture_with_filament_modifiers(
        model, fixture, modifier_options, overlapping_modifier_options);

    Print print;
    prepare_print(print, model, config);
    return Slic3r::Test::gcode(print);
}

struct ExtrusionSegment {
    size_t index;
    double x0;
    double y0;
    double x1;
    double y1;
    double z;
    double extrusion;
    double xy_distance;
    double feedrate;
    ExtrusionRole role;
    bool perimeter;
};

void update_extrusion_role(const GCodeReader::GCodeLine &line, ExtrusionRole &role)
{
    const std::string_view comment = line.comment();
    for (const std::string_view marker : { std::string_view("TYPE:"), std::string_view("FEATURE:") }) {
        const size_t pos = comment.find(marker);
        if (pos != std::string_view::npos) {
            std::string_view value = comment.substr(pos + marker.size());
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
                value.remove_suffix(1);
            role = ExtrusionEntity::string_to_role(value);
            return;
        }
    }
}

std::vector<ExtrusionSegment> extrusion_segments(const std::string &gcode)
{
    std::vector<ExtrusionSegment> segments;
    GCodeReader reader;
    size_t index = 0;
    ExtrusionRole role = erNone;
    reader.parse_buffer(gcode, [&segments, &index, &role](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        update_extrusion_role(line, role);
        if (line.extruding(self) && line.dist_XY(self) > 0.f) {
            segments.push_back({
                index,
                static_cast<double>(self.x()),
                static_cast<double>(self.y()),
                static_cast<double>(line.new_X(self)),
                static_cast<double>(line.new_Y(self)),
                static_cast<double>(self.z()),
                static_cast<double>(line.dist_E(self)),
                static_cast<double>(line.dist_XY(self)),
                static_cast<double>(line.new_F(self)),
                role,
                is_perimeter(role)
            });
        }
        ++index;
    });
    return segments;
}

struct GeometrySegment {
    double x0;
    double y0;
    double x1;
    double y1;
    double z;
    ExtrusionRole role;
};

bool almost_equal(double lhs, double rhs, double tolerance = 1e-4)
{
    return std::abs(lhs - rhs) <= tolerance;
}


std::vector<GeometrySegment> raw_geometry(const std::vector<ExtrusionSegment> &segments)
{
    std::vector<GeometrySegment> geometry;
    geometry.reserve(segments.size());
    for (const ExtrusionSegment &segment : segments)
        geometry.push_back({ segment.x0, segment.y0, segment.x1, segment.y1, segment.z, segment.role });
    return geometry;
}

bool same_geometry(const std::vector<GeometrySegment> &lhs, const std::vector<GeometrySegment> &rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        const GeometrySegment &left = lhs[index];
        const GeometrySegment &right = rhs[index];
        if (left.role != right.role ||
            !almost_equal(left.x0, right.x0) || !almost_equal(left.y0, right.y0) ||
            !almost_equal(left.x1, right.x1) || !almost_equal(left.y1, right.y1) ||
            !almost_equal(left.z, right.z))
            return false;
    }
    return true;
}

std::vector<GeometrySegment> travel_geometry(const std::string &gcode)
{
    std::vector<GeometrySegment> travels;
    GCodeReader reader;
    ExtrusionRole role = erNone;
    reader.parse_buffer(gcode, [&travels, &role](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        update_extrusion_role(line, role);
        if (line.travel() && line.dist_XY(self) > 0.f) {
            travels.push_back({
                static_cast<double>(self.x()),
                static_cast<double>(self.y()),
                static_cast<double>(line.new_X(self)),
                static_cast<double>(line.new_Y(self)),
                static_cast<double>(self.z()),
                role
            });
        }
    });
    return travels;
}

std::vector<GeometrySegment> external_perimeter_seam_starts(const std::string &gcode)
{
    std::vector<GeometrySegment> seam_starts;
    GCodeReader reader;
    ExtrusionRole role = erNone;
    bool follows_travel = true;
    reader.parse_buffer(gcode, [&seam_starts, &role, &follows_travel](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        update_extrusion_role(line, role);
        if (line.travel() && line.dist_XY(self) > 0.f) {
            follows_travel = true;
        } else if (line.extruding(self) && line.dist_XY(self) > 0.f) {
            if (role == erExternalPerimeter && follows_travel) {
                seam_starts.push_back({
                    static_cast<double>(self.x()),
                    static_cast<double>(self.y()),
                    static_cast<double>(line.new_X(self)),
                    static_cast<double>(line.new_Y(self)),
                    static_cast<double>(self.z()),
                    role
                });
            }
            follows_travel = false;
        }
    });
    return seam_starts;
}

enum class ModifierRegion {
    Modifier,
    Normal,
    Boundary
};

double modifier_x_split(const std::vector<ExtrusionSegment> &segments)
{
    double min_x = segments.front().x0;
    double max_x = min_x;
    for (const ExtrusionSegment &segment : segments) {
        min_x = std::min({ min_x, segment.x0, segment.x1 });
        max_x = std::max({ max_x, segment.x0, segment.x1 });
    }
    return (min_x + max_x) * 0.5;
}

ModifierRegion modifier_region(const ExtrusionSegment &segment, double split)
{
    constexpr double margin = 0.05;
    if (std::max(segment.x0, segment.x1) < split - margin)
        return ModifierRegion::Modifier;
    if (std::min(segment.x0, segment.x1) > split + margin)
        return ModifierRegion::Normal;
    return ModifierRegion::Boundary;
}

bool has_modifier_and_normal_on_same_layer(const std::vector<ExtrusionSegment> &segments, double split)
{
    std::set<int> modifier_layers;
    for (const ExtrusionSegment &segment : segments) {
        if (modifier_region(segment, split) == ModifierRegion::Modifier)
            modifier_layers.insert(static_cast<int>(std::lround(segment.z * 1000.)));
    }
    for (const ExtrusionSegment &segment : segments) {
        if (modifier_region(segment, split) == ModifierRegion::Normal &&
            modifier_layers.count(static_cast<int>(std::lround(segment.z * 1000.))) != 0)
            return true;
    }
    return false;
}

double max_perimeter_feedrate(const std::vector<ExtrusionSegment> &segments, ModifierRegion region, double split)
{
    double max_feedrate = 0.;
    for (const ExtrusionSegment &segment : segments) {
        if (segment.perimeter && modifier_region(segment, split) == region)
            max_feedrate = std::max(max_feedrate, segment.feedrate);
    }
    return max_feedrate;
}

double perimeter_extrusion_per_xy(const std::vector<ExtrusionSegment> &segments, ModifierRegion region, double split)
{
    double extrusion = 0.;
    double distance = 0.;
    for (const ExtrusionSegment &segment : segments) {
        if (segment.perimeter && modifier_region(segment, split) == region) {
            extrusion += segment.extrusion;
            distance += segment.xy_distance;
        }
    }
    return extrusion / distance;
}

struct GCodeCommand {
    size_t index;
    double value;
};

std::vector<GCodeCommand> gcode_commands(const std::string &gcode, const char *command, char axis)
{
    std::vector<GCodeCommand> commands;
    GCodeReader reader;
    size_t index = 0;
    reader.parse_buffer(gcode, [&commands, &index, command, axis](GCodeReader &, const GCodeReader::GCodeLine &line) {
        float value = 0.;
        if (line.cmd_is(command) && line.has_value(axis, value))
            commands.push_back({ index, static_cast<double>(value) });
        ++index;
    });
    return commands;
}

std::vector<size_t> gcode_command_indices(const std::string &gcode, const char *command)
{
    std::vector<size_t> indices;
    GCodeReader reader;
    size_t index = 0;
    reader.parse_buffer(gcode, [&indices, &index, command](GCodeReader &, const GCodeReader::GCodeLine &line) {
        if (line.cmd_is(command))
            indices.push_back(index);
        ++index;
    });
    return indices;
}

struct ToolGCodeCommand {
    size_t index;
    int tool;
    double value;
};

std::vector<ToolGCodeCommand> pressure_advance_commands_by_tool(const std::string &gcode)
{
    std::vector<ToolGCodeCommand> commands;
    GCodeReader reader;
    size_t index = 0;
    int tool = 0;
    reader.parse_buffer(gcode, [&commands, &index, &tool](GCodeReader &, const GCodeReader::GCodeLine &line) {
        const std::string_view command = line.cmd();
        if (command.size() > 1 && command.front() == 'T')
            tool = std::stoi(std::string(command.substr(1)));
        float value = 0.;
        if (line.cmd_is("M900") && line.has_value('K', value))
            commands.push_back({ index, tool, static_cast<double>(value) });
        ++index;
    });
    return commands;
}

struct PathEvents {
    std::vector<size_t> travels;
    std::vector<size_t> wipes;
    std::vector<size_t> unretracts;
};

PathEvents path_events(const std::string &gcode)
{
    PathEvents events;
    GCodeReader reader;
    size_t index = 0;
    reader.parse_buffer(gcode, [&events, &index](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.travel() && line.dist_XY(self) > 0.f)
            events.travels.push_back(index);
        if (line.dist_E(self) > 0.f && line.dist_XY(self) == 0.f)
            events.unretracts.push_back(index);
        if (line.comment().find("WIPE_START") != std::string_view::npos)
            events.wipes.push_back(index);
        ++index;
    });
    return events;
}

std::vector<GCodeCommand> fan_commands(const std::string &gcode, bool auxiliary)
{
    std::vector<GCodeCommand> commands;
    GCodeReader reader;
    size_t index = 0;
    reader.parse_buffer(gcode, [&commands, &index, auxiliary](GCodeReader &, const GCodeReader::GCodeLine &line) {
        float pwm = 0.;
        float fan = 0.;
        const bool is_auxiliary = line.has_value('P', fan) && std::lround(fan) == 2;
        if (line.cmd_is("M106") && line.has_value('S', pwm) && is_auxiliary == auxiliary)
            commands.push_back({ index, static_cast<double>(pwm) });
        ++index;
    });
    return commands;
}

std::vector<GCodeCommand> fan_state_commands(const std::string &gcode, bool auxiliary)
{
    std::vector<GCodeCommand> commands;
    GCodeReader reader;
    size_t index = 0;
    reader.parse_buffer(gcode, [&commands, &index, auxiliary](GCodeReader &, const GCodeReader::GCodeLine &line) {
        float pwm = 0.;
        float fan = 0.;
        const bool is_auxiliary = line.has_value('P', fan) && std::lround(fan) == 2;
        if (is_auxiliary == auxiliary) {
            if (line.cmd_is("M106") && line.has_value('S', pwm))
                commands.push_back({ index, static_cast<double>(pwm) });
            else if (line.cmd_is("M107"))
                commands.push_back({ index, 0. });
        }
        ++index;
    });
    return commands;
}

bool has_command_value(const std::vector<GCodeCommand> &commands, double expected)
{
    return std::any_of(commands.begin(), commands.end(), [expected](const GCodeCommand &command) {
        return std::abs(command.value - expected) < 0.01;
    });
}

bool command_starts_region(const GCodeCommand &command,
                           const std::vector<ExtrusionSegment> &segments,
                           ModifierRegion expected_region,
                           double split)
{
    for (const ExtrusionSegment &segment : segments) {
        if (segment.index <= command.index)
            continue;
        const ModifierRegion region = modifier_region(segment, split);
        if (region != ModifierRegion::Boundary)
            return region == expected_region;
    }
    return false;
}

bool command_follows_region(const GCodeCommand &command,
                            const std::vector<ExtrusionSegment> &segments,
                            ModifierRegion expected_region,
                            double split)
{
    for (auto segment = segments.rbegin(); segment != segments.rend(); ++segment) {
        if (segment->index >= command.index)
            continue;
        const ModifierRegion region = modifier_region(*segment, split);
        if (region != ModifierRegion::Boundary)
            return region == expected_region;
    }
    return false;
}

std::vector<double> command_values(const std::vector<GCodeCommand> &commands)
{
    std::vector<double> values;
    values.reserve(commands.size());
    for (const GCodeCommand &command : commands)
        values.push_back(command.value);
    return values;
}

void configure_modifier_cooling(DynamicPrintConfig &config, bool auxiliary_fan)
{
    config.set_deserialize_strict({
        { "fan_min_speed",                 "20" },
        { "fan_max_speed",                 "20" },
        { "close_fan_the_first_x_layers",  "0" },
        { "auxiliary_fan",                 auxiliary_fan },
        { "additional_cooling_fan_speed",  "20" }
    });
}

struct ModifierBounds {
    double min_x;
    double max_x;
};

ModifierBounds modifier_bounds(const std::vector<ExtrusionSegment> &segments, double modifier_width)
{
    double min_x = segments.front().x0;
    double max_x = min_x;
    for (const ExtrusionSegment &segment : segments) {
        min_x = std::min({ min_x, segment.x0, segment.x1 });
        max_x = std::max({ max_x, segment.x0, segment.x1 });
    }

    const double width = (max_x - min_x) * modifier_width / 20.;
    return { min_x, min_x + width };
}

ModifierRegion modifier_region(const ExtrusionSegment &segment, const ModifierBounds &bounds)
{
    constexpr double margin = 1e-3;
    const double min_x = std::min(segment.x0, segment.x1);
    const double max_x = std::max(segment.x0, segment.x1);
    if (min_x >= bounds.min_x - margin && max_x <= bounds.max_x + margin)
        return ModifierRegion::Modifier;
    if (max_x <= bounds.min_x + margin || min_x >= bounds.max_x - margin)
        return ModifierRegion::Normal;
    return ModifierRegion::Boundary;
}

constexpr std::array<ExtrusionRole, 8> object_extrusion_roles {
    erPerimeter,
    erExternalPerimeter,
    erInternalInfill,
    erSolidInfill,
    erTopSolidInfill,
    erBottomSurface,
    erInternalBridgeInfill,
    erIroning
};

void configure_multi_role_infill(DynamicPrintConfig &config)
{
    config.set_deserialize_strict({
        { "wall_loops",                    2 },
        { "sparse_infill_density",         15 },
        { "top_shell_layers",              2 },
        { "bottom_shell_layers",           2 },
        { "extra_solid_infills",           "10" },
        { "dont_filter_internal_bridges",  "nofilter" },
        { "ironing_type",                  "top" }
    });
}

bool has_role_in_region(const std::vector<ExtrusionSegment> &segments,
                        ExtrusionRole role,
                        ModifierRegion region,
                        const ModifierBounds &bounds)
{
    return std::any_of(segments.begin(), segments.end(), [role, region, &bounds](const ExtrusionSegment &segment) {
        return segment.role == role && modifier_region(segment, bounds) == region;
    });
}

std::vector<ExtrusionSegment> model_segments(const std::vector<ExtrusionSegment> &segments)
{
    std::vector<ExtrusionSegment> model;
    std::copy_if(
        segments.begin(), segments.end(), std::back_inserter(model),
        [](const ExtrusionSegment &segment) {
            return segment.role >= erPerimeter && segment.role <= erGapFill;
        });
    return model;
}

std::set<ExtrusionRole> roles_in_both_regions(const std::vector<ExtrusionSegment> &segments,
                                              const ModifierBounds &bounds)
{
    std::set<ExtrusionRole> modifier_roles;
    std::set<ExtrusionRole> normal_roles;
    for (const ExtrusionSegment &segment : segments) {
        const ModifierRegion region = modifier_region(segment, bounds);
        if (region == ModifierRegion::Modifier)
            modifier_roles.insert(segment.role);
        else if (region == ModifierRegion::Normal)
            normal_roles.insert(segment.role);
    }

    std::set<ExtrusionRole> both;
    std::set_intersection(
        modifier_roles.begin(), modifier_roles.end(),
        normal_roles.begin(), normal_roles.end(),
        std::inserter(both, both.end()));
    return both;
}

bool has_boundary_segment(const std::vector<ExtrusionSegment> &segments, const ModifierBounds &bounds)
{
    return std::any_of(segments.begin(), segments.end(), [&bounds](const ExtrusionSegment &segment) {
        return segment.role >= erPerimeter && segment.role <= erGapFill &&
               modifier_region(segment, bounds) == ModifierRegion::Boundary;
    });
}

double extrusion_per_xy(const std::vector<ExtrusionSegment> &segments,
                        ExtrusionRole role,
                        ModifierRegion region,
                        const ModifierBounds &bounds)
{
    double extrusion = 0.;
    double distance = 0.;
    for (const ExtrusionSegment &segment : segments) {
        if (segment.role == role && modifier_region(segment, bounds) == region) {
            extrusion += segment.extrusion;
            distance += segment.xy_distance;
        }
    }
    return extrusion / distance;
}

double max_feedrate(const std::vector<ExtrusionSegment> &segments,
                    ExtrusionRole role,
                    ModifierRegion region,
                    const ModifierBounds &bounds)
{
    double feedrate = 0.;
    for (const ExtrusionSegment &segment : segments) {
        if (segment.role == role && modifier_region(segment, bounds) == region)
            feedrate = std::max(feedrate, segment.feedrate);
    }
    return feedrate;
}

bool command_starts_role_region(const std::vector<GCodeCommand> &commands,
                                double value,
                                const std::vector<ExtrusionSegment> &segments,
                                ExtrusionRole role,
                                ModifierRegion region,
                                const ModifierBounds &bounds)
{
    for (const GCodeCommand &command : commands) {
        if (!almost_equal(command.value, value, 0.01))
            continue;
        const auto next_segment = std::find_if(
            segments.begin(), segments.end(),
            [&command](const ExtrusionSegment &segment) { return segment.index > command.index; });
        if (next_segment != segments.end() &&
            next_segment->role == role &&
            modifier_region(*next_segment, bounds) == region)
            return true;
    }
    return false;
}

bool command_is_active_for_role_region(const std::vector<GCodeCommand> &commands,
                                       double value,
                                       const std::vector<ExtrusionSegment> &segments,
                                       ExtrusionRole role,
                                       ModifierRegion region,
                                       const ModifierBounds &bounds)
{
    auto command = commands.begin();
    const GCodeCommand *active = nullptr;
    for (const ExtrusionSegment &segment : segments) {
        while (command != commands.end() && command->index < segment.index)
            active = &*command++;
        if (segment.role == role && modifier_region(segment, bounds) == region &&
            active != nullptr && almost_equal(active->value, value, 0.01))
            return true;
    }
    return false;
}

bool command_follows_role_region(const GCodeCommand &command,
                                 const std::vector<ExtrusionSegment> &segments,
                                 ExtrusionRole role,
                                 ModifierRegion region,
                                 const ModifierBounds &bounds)
{
    const auto previous_segment = std::find_if(
        segments.rbegin(), segments.rend(),
        [&command](const ExtrusionSegment &segment) { return segment.index < command.index; });
    return previous_segment != segments.rend() &&
           previous_segment->role == role &&
           modifier_region(*previous_segment, bounds) == region;
}

} // namespace

TEST_CASE("Filament modifier volume config flows to print region config", "[FilamentModifier][PrintObject]")
{
    Model model;
    ModelObject *object = model.add_object();
    add_cube_model_part(*object);
    ModelVolume *modifier = add_filament_modifier_cube(*object);
    modifier->config.set_key_value("modifier_nozzle_temperature", new ConfigOptionInt(230));
    modifier->config.set_key_value("modifier_max_volumetric_speed", new ConfigOptionFloat(0.6));
    modifier->config.set_key_value("modifier_pressure_advance", new ConfigOptionFloat(0.8));
    modifier->config.set_key_value("print_flow_ratio", new ConfigOptionFloat(1.5));
    modifier->config.set_key_value("modifier_fan_speed", new ConfigOptionInt(80));
    modifier->config.set_key_value("modifier_aux_fan_speed", new ConfigOptionInt(70));

    PrintRegionConfig region_config = region_config_from_model_volume(PrintRegionConfig{}, nullptr, *modifier, 1);

    REQUIRE(region_config.modifier_nozzle_temperature.value == 230);
    REQUIRE(region_config.modifier_max_volumetric_speed.value == Catch::Approx(0.6));
    REQUIRE(region_config.modifier_pressure_advance.value == Catch::Approx(0.8));
    REQUIRE(region_config.print_flow_ratio.value == Catch::Approx(1.5));
    REQUIRE(region_config.modifier_fan_speed.value == 80);
    REQUIRE(region_config.modifier_aux_fan_speed.value == 70);
}

TEST_CASE("Filament modifier scope enum uses stable serialized values", "[FilamentModifier][Config]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    REQUIRE(config.opt_enum<FilamentModifierScope>("filament_modifier_scope") == FilamentModifierScope::Model);

    for (const auto &[scope, serialized] : std::array<std::pair<FilamentModifierScope, const char *>, 3> {{
             { FilamentModifierScope::Model, "model" },
             { FilamentModifierScope::ModelSupport, "model_support" },
             { FilamentModifierScope::ModelSupportAdhesion, "model_support_adhesion" }
         }}) {
        ConfigOptionEnum<FilamentModifierScope> option(scope);
        REQUIRE(option.serialize() == serialized);
        ConfigOptionEnum<FilamentModifierScope> round_trip;
        REQUIRE(round_trip.deserialize(serialized));
        REQUIRE(round_trip.value == scope);
    }
}

TEST_CASE("Filament modifier scopes volumetric feedrate on a partial model", "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();

    const std::string uncapped_gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "modifier_max_volumetric_speed", new ConfigOptionFloat(20.) }});
    const std::string capped_gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "modifier_max_volumetric_speed", new ConfigOptionFloat(0.6) }});

    const std::vector<ExtrusionSegment> uncapped_segments = extrusion_segments(uncapped_gcode);
    const std::vector<ExtrusionSegment> capped_segments = extrusion_segments(capped_gcode);
    REQUIRE(!uncapped_segments.empty());
    REQUIRE(!capped_segments.empty());

    const double uncapped_split = modifier_x_split(uncapped_segments);
    const double capped_split = modifier_x_split(capped_segments);
    REQUIRE(has_modifier_and_normal_on_same_layer(capped_segments, capped_split));

    const double uncapped_modifier_feedrate = max_perimeter_feedrate(uncapped_segments, ModifierRegion::Modifier, uncapped_split);
    const double uncapped_normal_feedrate = max_perimeter_feedrate(uncapped_segments, ModifierRegion::Normal, uncapped_split);
    const double capped_modifier_feedrate = max_perimeter_feedrate(capped_segments, ModifierRegion::Modifier, capped_split);
    const double capped_normal_feedrate = max_perimeter_feedrate(capped_segments, ModifierRegion::Normal, capped_split);

    REQUIRE(uncapped_modifier_feedrate > 0.);
    REQUIRE(uncapped_normal_feedrate > 0.);
    REQUIRE(capped_modifier_feedrate > 0.);
    REQUIRE(capped_normal_feedrate > 0.);
    REQUIRE(capped_modifier_feedrate < capped_normal_feedrate);
    REQUIRE(capped_modifier_feedrate < uncapped_modifier_feedrate);
    REQUIRE(capped_normal_feedrate == Catch::Approx(uncapped_normal_feedrate).epsilon(0.01));
}

TEST_CASE("Filament modifier emits and restores nozzle temperature spatially", "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();

    const std::string modified_gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "modifier_nozzle_temperature", new ConfigOptionInt(230) }});
    const std::vector<ExtrusionSegment> segments = extrusion_segments(modified_gcode);
    REQUIRE(!segments.empty());
    const double split = modifier_x_split(segments);

    const std::vector<GCodeCommand> temperatures = gcode_commands(modified_gcode, "M104", 'S');
    const auto override = std::find_if(temperatures.begin(), temperatures.end(), [](const GCodeCommand &command) {
        return std::abs(command.value - 230.) < 0.01;
    });
    REQUIRE(override != temperatures.end());
    const auto restore = std::find_if(override + 1, temperatures.end(), [&segments, split](const GCodeCommand &command) {
        return std::abs(command.value - 220.) < 0.01 &&
               command_follows_region(command, segments, ModifierRegion::Modifier, split);
    });
    REQUIRE(restore != temperatures.end());
    REQUIRE(command_starts_region(*override, segments, ModifierRegion::Modifier, split));
    const auto first_normal = std::find_if(segments.begin(), segments.end(), [override, split](const ExtrusionSegment &segment) {
        return segment.index > override->index && modifier_region(segment, split) == ModifierRegion::Normal;
    });
    REQUIRE(first_normal != segments.end());
    REQUIRE(restore->index < first_normal->index);

    const std::string inherited_gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "modifier_nozzle_temperature", new ConfigOptionInt(0) }});
    const std::vector<GCodeCommand> inherited_temperatures = gcode_commands(inherited_gcode, "M104", 'S');
    REQUIRE(has_command_value(inherited_temperatures, 220.));
    REQUIRE_FALSE(has_command_value(inherited_temperatures, 230.));
}

TEST_CASE("Filament modifier scopes print flow ratio and restores normal extrusion", "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();

    const std::string base_gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "modifier_max_volumetric_speed", new ConfigOptionFloat(20.) }});
    const std::string modified_gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "print_flow_ratio", new ConfigOptionFloat(1.5) }});
    const std::vector<ExtrusionSegment> base_segments = extrusion_segments(base_gcode);
    const std::vector<ExtrusionSegment> modified_segments = extrusion_segments(modified_gcode);
    REQUIRE(!base_segments.empty());
    REQUIRE(!modified_segments.empty());

    const double base_split = modifier_x_split(base_segments);
    const double modified_split = modifier_x_split(modified_segments);
    const double base_modifier_flow = perimeter_extrusion_per_xy(base_segments, ModifierRegion::Modifier, base_split);
    const double base_normal_flow = perimeter_extrusion_per_xy(base_segments, ModifierRegion::Normal, base_split);
    const double modified_modifier_flow = perimeter_extrusion_per_xy(modified_segments, ModifierRegion::Modifier, modified_split);
    const double modified_normal_flow = perimeter_extrusion_per_xy(modified_segments, ModifierRegion::Normal, modified_split);

    REQUIRE(base_modifier_flow > 0.);
    REQUIRE(base_normal_flow > 0.);
    REQUIRE(modified_modifier_flow > 0.);
    REQUIRE(modified_normal_flow > 0.);
    REQUIRE(modified_modifier_flow / base_modifier_flow == Catch::Approx(1.5).epsilon(0.02));
    REQUIRE(modified_normal_flow / base_normal_flow == Catch::Approx(1.).epsilon(0.02));
}

TEST_CASE("Filament modifier emits and restores Marlin pressure advance", "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    config.set_deserialize_strict({
        { "gcode_flavor",             "marlin" },
        { "enable_pressure_advance",  "1" },
        { "pressure_advance",         "0.2" }
    });

    const std::string modified_gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "modifier_pressure_advance", new ConfigOptionFloat(0.8) }});
    const std::vector<ExtrusionSegment> segments = extrusion_segments(modified_gcode);
    REQUIRE(!segments.empty());
    const double split = modifier_x_split(segments);

    const std::vector<GCodeCommand> pressure_advances = gcode_commands(modified_gcode, "M900", 'K');
    const auto override = std::find_if(pressure_advances.begin(), pressure_advances.end(), [](const GCodeCommand &command) {
        return std::abs(command.value - 0.8) < 0.01;
    });
    REQUIRE(override != pressure_advances.end());
    const auto base_before_override = std::find_if(pressure_advances.begin(), override, [](const GCodeCommand &command) {
        return std::abs(command.value - 0.2) < 0.01;
    });
    REQUIRE(base_before_override != override);
    const auto restore = std::find_if(override + 1, pressure_advances.end(), [&segments, split](const GCodeCommand &command) {
        return std::abs(command.value - 0.2) < 0.01 &&
               command_follows_region(command, segments, ModifierRegion::Modifier, split);
    });
    REQUIRE(restore != pressure_advances.end());
    REQUIRE(command_starts_region(*override, segments, ModifierRegion::Modifier, split));
    const auto first_normal = std::find_if(segments.begin(), segments.end(), [override, split](const ExtrusionSegment &segment) {
        return segment.index > override->index && modifier_region(segment, split) == ModifierRegion::Normal;
    });
    REQUIRE(first_normal != segments.end());
    REQUIRE(restore->index < first_normal->index);

    const std::string inherited_gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "modifier_pressure_advance", new ConfigOptionFloat(-1.) }});
    const std::vector<GCodeCommand> inherited_pressure_advances = gcode_commands(inherited_gcode, "M900", 'K');
    REQUIRE(!inherited_pressure_advances.empty());
    REQUIRE(std::all_of(inherited_pressure_advances.begin(), inherited_pressure_advances.end(),
                        [](const GCodeCommand &command) { return std::abs(command.value - 0.2) < 0.01; }));
}

TEST_CASE("Filament modifier emits and restores the main fan", "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    configure_modifier_cooling(config, true);

    const std::string modified_gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "modifier_fan_speed", new ConfigOptionInt(5) }});
    const std::vector<ExtrusionSegment> segments = extrusion_segments(modified_gcode);
    REQUIRE(!segments.empty());
    const double split = modifier_x_split(segments);

    const std::vector<GCodeCommand> main_fans = fan_commands(modified_gcode, false);
    const auto override = std::find_if(main_fans.begin(), main_fans.end(), [](const GCodeCommand &command) {
        return std::abs(command.value - 12.) < 0.01;
    });
    REQUIRE(override != main_fans.end());
    REQUIRE(override != main_fans.begin());
    REQUIRE(std::abs((override - 1)->value - 51.) < 0.01);
    REQUIRE(command_starts_region(*override, segments, ModifierRegion::Modifier, split));
    REQUIRE(override + 1 != main_fans.end());
    REQUIRE(std::abs((override + 1)->value - 51.) < 0.01);
    REQUIRE(command_follows_region(*(override + 1), segments, ModifierRegion::Modifier, split));

    const std::vector<GCodeCommand> control_fans = fan_commands(
        slice_cube_with_optional_filament_modifier(config),
        false);
    const std::vector<GCodeCommand> inherited_fans = fan_commands(
        slice_cube_with_optional_filament_modifier(
            config,
            10.,
            {{ "modifier_fan_speed", new ConfigOptionInt(-1) }}),
        false);
    REQUIRE(command_values(inherited_fans) == command_values(control_fans));
}

TEST_CASE("Filament modifier emits and restores the auxiliary fan", "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    configure_modifier_cooling(config, true);

    const std::string modified_gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "modifier_aux_fan_speed", new ConfigOptionInt(5) }});
    const std::vector<ExtrusionSegment> segments = extrusion_segments(modified_gcode);
    REQUIRE(!segments.empty());
    const double split = modifier_x_split(segments);

    const std::vector<GCodeCommand> auxiliary_fans = fan_commands(modified_gcode, true);
    const auto override = std::find_if(auxiliary_fans.begin(), auxiliary_fans.end(), [](const GCodeCommand &command) {
        return std::abs(command.value - 12.) < 0.01;
    });
    REQUIRE(override != auxiliary_fans.end());
    REQUIRE(override != auxiliary_fans.begin());
    REQUIRE(std::abs((override - 1)->value - 51.) < 0.01);
    REQUIRE(command_starts_region(*override, segments, ModifierRegion::Modifier, split));
    REQUIRE(override + 1 != auxiliary_fans.end());
    REQUIRE(std::abs((override + 1)->value - 51.) < 0.01);
    REQUIRE(command_follows_region(*(override + 1), segments, ModifierRegion::Modifier, split));

    const std::vector<GCodeCommand> control_fans = fan_commands(
        slice_cube_with_optional_filament_modifier(config),
        true);
    const std::vector<GCodeCommand> inherited_fans = fan_commands(
        slice_cube_with_optional_filament_modifier(
            config,
            10.,
            {{ "modifier_aux_fan_speed", new ConfigOptionInt(-1) }}),
        true);
    REQUIRE(command_values(inherited_fans) == command_values(control_fans));

    DynamicPrintConfig disabled_config = filament_modifier_test_config();
    configure_modifier_cooling(disabled_config, false);
    const std::vector<GCodeCommand> disabled_auxiliary_fans = fan_commands(
        slice_cube_with_optional_filament_modifier(
            disabled_config,
            10.,
            {{ "modifier_aux_fan_speed", new ConfigOptionInt(5) }}),
        true);
    REQUIRE(disabled_auxiliary_fans.empty());
}

TEST_CASE("Filament modifier with explicit neutral values preserves paths and commands", "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    configure_modifier_cooling(config, true);
    config.set_deserialize_strict({
        { "gcode_flavor",             "marlin" },
        { "enable_pressure_advance",  "1" },
        { "pressure_advance",         "0.2" }
    });

    const std::string control_gcode = slice_cube_with_optional_filament_modifier(config);
    const std::string neutral_gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {
            { "modifier_nozzle_temperature",   new ConfigOptionInt(0) },
            { "modifier_max_volumetric_speed", new ConfigOptionFloat(0.) },
            { "modifier_pressure_advance",     new ConfigOptionFloat(-1.) },
            { "print_flow_ratio",               new ConfigOptionFloat(1.) },
            { "modifier_fan_speed",             new ConfigOptionInt(-1) },
            { "modifier_aux_fan_speed",         new ConfigOptionInt(-1) }
        });

    const std::vector<ExtrusionSegment> control_segments = extrusion_segments(control_gcode);
    const std::vector<ExtrusionSegment> neutral_segments = extrusion_segments(neutral_gcode);
    REQUIRE(same_geometry(raw_geometry(control_segments), raw_geometry(neutral_segments)));
    REQUIRE(same_geometry(travel_geometry(control_gcode), travel_geometry(neutral_gcode)));
    REQUIRE(same_geometry(
        external_perimeter_seam_starts(control_gcode),
        external_perimeter_seam_starts(neutral_gcode)));
    REQUIRE(command_values(gcode_commands(control_gcode, "M104", 'S')) ==
            command_values(gcode_commands(neutral_gcode, "M104", 'S')));
    REQUIRE(command_values(gcode_commands(control_gcode, "M900", 'K')) ==
            command_values(gcode_commands(neutral_gcode, "M900", 'K')));
    REQUIRE(command_values(fan_commands(control_gcode, false)) ==
            command_values(fan_commands(neutral_gcode, false)));
    REQUIRE(command_values(fan_commands(control_gcode, true)) ==
            command_values(fan_commands(neutral_gcode, true)));
}

TEST_CASE("Active filament modifier splits exactly at strict volume boundaries", "[FilamentModifier][GCode]")
{
    constexpr double modifier_width = 10.;
    DynamicPrintConfig config = filament_modifier_test_config();
    configure_multi_role_infill(config);

    const std::string control_gcode = slice_cube_with_optional_filament_modifier(config);
    const std::string baseline_gcode = slice_cube_with_optional_filament_modifier(
        config,
        modifier_width,
        {{ "modifier_max_volumetric_speed", new ConfigOptionFloat(20.) }});
    const std::string modified_gcode = slice_cube_with_optional_filament_modifier(
        config,
        modifier_width,
        {{ "print_flow_ratio", new ConfigOptionFloat(1.5) }});
    const std::vector<ExtrusionSegment> control = extrusion_segments(control_gcode);
    const std::vector<ExtrusionSegment> baseline = extrusion_segments(baseline_gcode);
    const std::vector<ExtrusionSegment> modified = extrusion_segments(modified_gcode);
    REQUIRE(!control.empty());
    REQUIRE(!baseline.empty());
    REQUIRE(!modified.empty());

    const ModifierBounds control_bounds = modifier_bounds(control, modifier_width);
    const ModifierBounds baseline_bounds = modifier_bounds(baseline, modifier_width);
    const ModifierBounds modified_bounds = modifier_bounds(modified, modifier_width);
    REQUIRE(has_boundary_segment(control, control_bounds));
    REQUIRE_FALSE(has_boundary_segment(baseline, baseline_bounds));
    REQUIRE_FALSE(has_boundary_segment(modified, modified_bounds));
    REQUIRE(same_geometry(raw_geometry(baseline), raw_geometry(modified)));
    // Active modifiers deliberately partition and reorder paths at their boundary.
    REQUIRE(same_geometry(travel_geometry(baseline_gcode), travel_geometry(modified_gcode)));
    REQUIRE(same_geometry(
        external_perimeter_seam_starts(baseline_gcode),
        external_perimeter_seam_starts(modified_gcode)));
}

TEST_CASE("Filament modifier scopes flow and volumetric speed to every object extrusion role", "[FilamentModifier][GCode]")
{
    constexpr double modifier_width = 10.;
    DynamicPrintConfig config = filament_modifier_test_config();
    configure_multi_role_infill(config);

    const std::vector<ExtrusionSegment> baseline = extrusion_segments(
        slice_cube_with_optional_filament_modifier(
            config,
            modifier_width,
            {{ "modifier_max_volumetric_speed", new ConfigOptionFloat(20.) }}));
    const std::vector<ExtrusionSegment> flow_modified = extrusion_segments(
        slice_cube_with_optional_filament_modifier(
            config,
            modifier_width,
            {{ "print_flow_ratio", new ConfigOptionFloat(1.5) }}));
    const std::vector<ExtrusionSegment> speed_modified = extrusion_segments(
        slice_cube_with_optional_filament_modifier(
            config,
            modifier_width,
            {{ "modifier_max_volumetric_speed", new ConfigOptionFloat(0.01) }}));
    REQUIRE(!baseline.empty());
    REQUIRE(!flow_modified.empty());
    REQUIRE(!speed_modified.empty());

    const ModifierBounds baseline_bounds = modifier_bounds(baseline, modifier_width);
    const ModifierBounds flow_bounds = modifier_bounds(flow_modified, modifier_width);
    const ModifierBounds speed_bounds = modifier_bounds(speed_modified, modifier_width);
    const std::set<ExtrusionRole> generated_roles = roles_in_both_regions(baseline, baseline_bounds);
    for (const ExtrusionRole role : object_extrusion_roles) {
        INFO("Required role: " << ExtrusionEntity::role_to_string(role));
        REQUIRE(generated_roles.count(role) == 1);
    }

    for (const ExtrusionRole role : generated_roles) {
        if (role < erPerimeter || role > erGapFill)
            continue;
        INFO("Role: " << ExtrusionEntity::role_to_string(role));
        REQUIRE(has_role_in_region(baseline, role, ModifierRegion::Modifier, baseline_bounds));
        REQUIRE(has_role_in_region(baseline, role, ModifierRegion::Normal, baseline_bounds));
        REQUIRE(has_role_in_region(flow_modified, role, ModifierRegion::Modifier, flow_bounds));
        REQUIRE(has_role_in_region(flow_modified, role, ModifierRegion::Normal, flow_bounds));
        REQUIRE(has_role_in_region(speed_modified, role, ModifierRegion::Modifier, speed_bounds));
        REQUIRE(has_role_in_region(speed_modified, role, ModifierRegion::Normal, speed_bounds));

        const double baseline_modifier_flow = extrusion_per_xy(
            baseline, role, ModifierRegion::Modifier, baseline_bounds);
        const double baseline_normal_flow = extrusion_per_xy(
            baseline, role, ModifierRegion::Normal, baseline_bounds);
        const double modifier_flow = extrusion_per_xy(
            flow_modified, role, ModifierRegion::Modifier, flow_bounds);
        const double normal_flow = extrusion_per_xy(
            flow_modified, role, ModifierRegion::Normal, flow_bounds);
        REQUIRE(modifier_flow / baseline_modifier_flow == Catch::Approx(1.5).epsilon(0.03));
        REQUIRE(normal_flow / baseline_normal_flow == Catch::Approx(1.).epsilon(0.03));

        const double baseline_modifier_speed = max_feedrate(
            baseline, role, ModifierRegion::Modifier, baseline_bounds);
        const double baseline_normal_speed = max_feedrate(
            baseline, role, ModifierRegion::Normal, baseline_bounds);
        const double modifier_speed = max_feedrate(
            speed_modified, role, ModifierRegion::Modifier, speed_bounds);
        const double normal_speed = max_feedrate(
            speed_modified, role, ModifierRegion::Normal, speed_bounds);
        REQUIRE(modifier_speed < baseline_modifier_speed);
        REQUIRE(normal_speed == Catch::Approx(baseline_normal_speed).epsilon(0.03));
    }
}

TEST_CASE("Filament modifier restores nozzle state for every object extrusion role", "[FilamentModifier][GCode]")
{
    constexpr double modifier_width = 10.;
    DynamicPrintConfig config = filament_modifier_test_config();
    configure_multi_role_infill(config);

    const std::string gcode = slice_cube_with_optional_filament_modifier(
        config,
        modifier_width,
        {{ "modifier_nozzle_temperature", new ConfigOptionInt(230) }});
    const std::vector<ExtrusionSegment> segments = extrusion_segments(gcode);
    REQUIRE(!segments.empty());
    const ModifierBounds bounds = modifier_bounds(segments, modifier_width);
    const std::vector<GCodeCommand> temperatures = gcode_commands(gcode, "M104", 'S');

    const std::set<ExtrusionRole> generated_roles = roles_in_both_regions(segments, bounds);
    for (const ExtrusionRole required_role : object_extrusion_roles)
        REQUIRE(generated_roles.count(required_role) == 1);

    for (const ExtrusionRole role : generated_roles) {
        if (role < erPerimeter || role > erGapFill)
            continue;
        INFO("Role: " << ExtrusionEntity::role_to_string(role));
        REQUIRE(command_starts_role_region(
            temperatures, 230., segments, role, ModifierRegion::Modifier, bounds));
        REQUIRE(std::any_of(temperatures.begin(), temperatures.end(), [&segments, role, &bounds](const GCodeCommand &command) {
            return almost_equal(command.value, 220., 0.01) &&
                   command_follows_role_region(command, segments, role, ModifierRegion::Modifier, bounds);
        }));
    }
}

TEST_CASE("Filament modifier reaches deterministic specialized model roles", "[FilamentModifier][GCode]")
{
    auto require_scoped_role = [](const DynamicPrintConfig &config, TestMesh fixture, ExtrusionRole role) {
        const std::string gcode = slice_fixture_with_filament_modifier(
            config,
            fixture,
            {{ "modifier_nozzle_temperature", new ConfigOptionInt(230) }});
        const std::vector<ExtrusionSegment> segments = extrusion_segments(gcode);
        const std::vector<ExtrusionSegment> object_segments = model_segments(segments);
        REQUIRE(!object_segments.empty());
        const ModifierBounds bounds = modifier_bounds(object_segments, 10.);
        REQUIRE(has_role_in_region(segments, role, ModifierRegion::Modifier, bounds));
        REQUIRE(has_role_in_region(segments, role, ModifierRegion::Normal, bounds));

        const std::vector<GCodeCommand> temperatures = gcode_commands(gcode, "M104", 'S');
        REQUIRE(command_starts_role_region(
            temperatures, 230., segments, role, ModifierRegion::Modifier, bounds));
        REQUIRE(std::any_of(temperatures.begin(), temperatures.end(), [&segments, role, &bounds](const GCodeCommand &command) {
            return almost_equal(command.value, 220., 0.01) &&
                   command_follows_role_region(command, segments, role, ModifierRegion::Modifier, bounds);
        }));
    };

    SECTION("external bridge") {
        DynamicPrintConfig config = filament_modifier_test_config();
        config.set_deserialize_strict({
            { "wall_loops",      2 },
            { "enable_support",  false }
        });
        require_scoped_role(config, TestMesh::bridge, erBridgeInfill);
    }

    SECTION("overhang perimeter") {
        DynamicPrintConfig config = filament_modifier_test_config();
        config.set_deserialize_strict({
            { "wall_loops",            2 },
            { "detect_overhang_wall",  true },
            { "enable_support",        false }
        });
        require_scoped_role(config, TestMesh::overhang, erOverhangPerimeter);
    }

    SECTION("classic gap fill") {
        DynamicPrintConfig config = filament_modifier_test_config();
        config.set_deserialize_strict({
            { "wall_generator",   "classic" },
            { "wall_loops",       3 },
            { "gap_fill_target",  "everywhere" }
        });
        require_scoped_role(config, TestMesh::gt2_teeth, erGapFill);
    }
}

TEST_CASE("Filament modifier scope controls model support and adhesion G-code", "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    config.set_deserialize_strict({
        { "enable_support",           true },
        { "support_threshold_angle",  45 },
        { "brim_type",                "outer_only" },
        { "brim_width",               5 },
        { "skirt_loops",              1 },
        { "skirt_height",             1 },
        { "skirt_distance",           1 }
    });

    auto slice_scope = [&config](FilamentModifierScope scope) {
        return slice_fixture_with_filament_modifier(
            config,
            TestMesh::overhang,
            {
                { "filament_modifier_scope", new ConfigOptionEnum<FilamentModifierScope>(scope) },
                { "modifier_nozzle_temperature", new ConfigOptionInt(230) }
            });
    };

    const std::string model_gcode = slice_scope(FilamentModifierScope::Model);
    const std::string support_gcode = slice_scope(FilamentModifierScope::ModelSupport);
    const std::string adhesion_gcode = slice_scope(FilamentModifierScope::ModelSupportAdhesion);
    const std::vector<ExtrusionSegment> adhesion_segments = extrusion_segments(adhesion_gcode);
    const std::vector<ExtrusionSegment> adhesion_object_segments = model_segments(adhesion_segments);
    REQUIRE(!adhesion_object_segments.empty());
    const ModifierBounds adhesion_bounds = modifier_bounds(adhesion_object_segments, 10.);
    const std::set<ExtrusionRole> scoped_roles = roles_in_both_regions(adhesion_segments, adhesion_bounds);

    std::set<ExtrusionRole> model_roles;
    std::set<ExtrusionRole> support_roles;
    std::set<ExtrusionRole> adhesion_roles;
    for (const ExtrusionRole role : scoped_roles) {
        if (role >= erPerimeter && role <= erGapFill)
            model_roles.insert(role);
        else if (is_support(role))
            support_roles.insert(role);
        else if (role == erSkirt || role == erBrim)
            adhesion_roles.insert(role);
    }
    REQUIRE(!model_roles.empty());
    REQUIRE(!support_roles.empty());
    REQUIRE(adhesion_roles.count(erSkirt) == 1);
    REQUIRE(adhesion_roles.count(erBrim) == 1);

    auto verify_scope = [&model_roles, &support_roles, &adhesion_roles](
                            const std::string &gcode,
                            bool expect_support,
                            bool expect_adhesion) {
        const std::vector<ExtrusionSegment> segments = extrusion_segments(gcode);
        const std::vector<ExtrusionSegment> object_segments = model_segments(segments);
        REQUIRE(!object_segments.empty());
        const ModifierBounds bounds = modifier_bounds(object_segments, 10.);
        const std::vector<GCodeCommand> temperatures = gcode_commands(gcode, "M104", 'S');

        auto verify_roles = [&temperatures, &segments, &bounds](
                                const std::set<ExtrusionRole> &roles,
                                bool expected) {
            for (const ExtrusionRole role : roles) {
                INFO("Role: " << ExtrusionEntity::role_to_string(role));
                const bool has_override = command_is_active_for_role_region(
                    temperatures, 230., segments, role, ModifierRegion::Modifier, bounds);
                REQUIRE(has_override == expected);
                if (role != erBrim)
                    REQUIRE_FALSE(command_is_active_for_role_region(
                        temperatures, 230., segments, role, ModifierRegion::Normal, bounds));
            }
        };

        verify_roles(model_roles, true);
        verify_roles(support_roles, expect_support);
        verify_roles(adhesion_roles, expect_adhesion);
    };

    verify_scope(model_gcode, false, false);
    verify_scope(support_gcode, true, false);
    verify_scope(adhesion_gcode, true, true);
}

TEST_CASE("Neutralizing an active filament modifier invalidates and rebuilds all scoped paths",
          "[FilamentModifier][Incremental]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    config.set_deserialize_strict({
        { "enable_support",           true },
        { "support_threshold_angle",  45 },
        { "brim_type",                "outer_only" },
        { "brim_width",               5 },
        { "skirt_loops",              1 },
        { "skirt_height",             1 },
        { "skirt_distance",           1 }
    });

    Model model;
    ModelVolume *modifier = add_fixture_with_filament_modifiers(
        model,
        TestMesh::overhang,
        {
            { "filament_modifier_scope",
              new ConfigOptionEnum<FilamentModifierScope>(FilamentModifierScope::ModelSupportAdhesion) },
            { "modifier_nozzle_temperature", new ConfigOptionInt(230) }
        });
    Print print;
    prepare_print(print, model, config);
    const std::string active_gcode = Slic3r::Test::gcode(print);
    REQUIRE(print.is_step_done(posPerimeters));
    REQUIRE(print.is_step_done(posSupportMaterial));
    REQUIRE(print.is_step_done(psSkirtBrim));
    REQUIRE(has_command_value(gcode_commands(active_gcode, "M104", 'S'), 230.));

    modifier->config.set_key_value("modifier_nozzle_temperature", new ConfigOptionInt(0));
    print.apply(model, config);
    REQUIRE_FALSE(print.is_step_done(posPerimeters));
    REQUIRE_FALSE(print.is_step_done(posSupportMaterial));
    REQUIRE_FALSE(print.is_step_done(psSkirtBrim));
    print.validate();
    print.set_status_silent();
    const std::string rebuilt_gcode = Slic3r::Test::gcode(print);
    const std::string fresh_neutral_gcode = slice_fixture_with_filament_modifier(
        config,
        TestMesh::overhang,
        {
            { "filament_modifier_scope",
              new ConfigOptionEnum<FilamentModifierScope>(FilamentModifierScope::ModelSupportAdhesion) },
            { "modifier_nozzle_temperature", new ConfigOptionInt(0) }
        });

    REQUIRE_FALSE(has_command_value(gcode_commands(rebuilt_gcode, "M104", 'S'), 230.));
    REQUIRE(command_values(gcode_commands(rebuilt_gcode, "M104", 'S')) ==
            command_values(gcode_commands(fresh_neutral_gcode, "M104", 'S')));
    REQUIRE(same_geometry(
        raw_geometry(extrusion_segments(rebuilt_gcode)),
        raw_geometry(extrusion_segments(fresh_neutral_gcode))));
    REQUIRE(same_geometry(
        travel_geometry(rebuilt_gcode),
        travel_geometry(fresh_neutral_gcode)));
}

TEST_CASE("Last overlapping filament modifier wins for model support and adhesion",
          "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    config.set_deserialize_strict({
        { "enable_support",           true },
        { "support_threshold_angle",  45 },
        { "brim_type",                "outer_only" },
        { "brim_width",               5 },
        { "skirt_loops",              1 },
        { "skirt_height",             1 },
        { "skirt_distance",           1 }
    });

    const std::string gcode = slice_fixture_with_filament_modifier(
        config,
        TestMesh::overhang,
        {
            { "filament_modifier_scope",
              new ConfigOptionEnum<FilamentModifierScope>(FilamentModifierScope::ModelSupportAdhesion) },
            { "modifier_nozzle_temperature", new ConfigOptionInt(230) }
        },
        {
            { "filament_modifier_scope",
              new ConfigOptionEnum<FilamentModifierScope>(FilamentModifierScope::ModelSupportAdhesion) },
            { "modifier_nozzle_temperature", new ConfigOptionInt(240) }
        });
    const std::vector<ExtrusionSegment> segments = extrusion_segments(gcode);
    const std::vector<ExtrusionSegment> object_segments = model_segments(segments);
    REQUIRE(!object_segments.empty());
    const ModifierBounds bounds = modifier_bounds(object_segments, 10.);
    const std::set<ExtrusionRole> scoped_roles = roles_in_both_regions(segments, bounds);
    const std::vector<GCodeCommand> temperatures = gcode_commands(gcode, "M104", 'S');

    bool saw_model = false;
    bool saw_support = false;
    bool saw_skirt = false;
    bool saw_brim = false;
    for (const ExtrusionRole role : scoped_roles) {
        const bool relevant =
            (role >= erPerimeter && role <= erGapFill) || is_support(role) || role == erSkirt || role == erBrim;
        if (!relevant)
            continue;
        INFO("Role: " << ExtrusionEntity::role_to_string(role));
        REQUIRE(command_is_active_for_role_region(
            temperatures, 240., segments, role, ModifierRegion::Modifier, bounds));
        REQUIRE_FALSE(command_is_active_for_role_region(
            temperatures, 230., segments, role, ModifierRegion::Modifier, bounds));
        saw_model |= role >= erPerimeter && role <= erGapFill;
        saw_support |= is_support(role);
        saw_skirt |= role == erSkirt;
        saw_brim |= role == erBrim;
    }
    REQUIRE(saw_model);
    REQUIRE(saw_support);
    REQUIRE(saw_skirt);
    REQUIRE(saw_brim);
    REQUIRE_FALSE(has_command_value(temperatures, 230.));
}

TEST_CASE("Static pressure advance is restored immediately for support and adhesion",
          "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    config.set_deserialize_strict({
        { "gcode_flavor",             "marlin" },
        { "enable_pressure_advance",  "1" },
        { "pressure_advance",         "0.2" },
        { "enable_support",           true },
        { "support_threshold_angle",  45 },
        { "brim_type",                "outer_only" },
        { "brim_width",               5 },
        { "skirt_loops",              1 },
        { "skirt_height",             1 },
        { "skirt_distance",           1 }
    });

    const std::string gcode = slice_fixture_with_filament_modifier(
        config,
        TestMesh::overhang,
        {
            { "filament_modifier_scope",
              new ConfigOptionEnum<FilamentModifierScope>(FilamentModifierScope::ModelSupportAdhesion) },
            { "modifier_pressure_advance", new ConfigOptionFloat(0.8) }
        });
    REQUIRE(gcode.find("; PA_CHANGE") == std::string::npos);

    const std::vector<ExtrusionSegment> segments = extrusion_segments(gcode);
    const std::vector<ExtrusionSegment> object_segments = model_segments(segments);
    REQUIRE(!object_segments.empty());
    const ModifierBounds bounds = modifier_bounds(object_segments, 10.);
    const std::set<ExtrusionRole> scoped_roles = roles_in_both_regions(segments, bounds);
    const std::vector<GCodeCommand> pressure_advances = gcode_commands(gcode, "M900", 'K');

    bool saw_support = false;
    bool saw_skirt = false;
    bool saw_brim = false;
    for (const ExtrusionRole role : scoped_roles) {
        if (!is_support(role) && role != erSkirt && role != erBrim)
            continue;
        INFO("Role: " << ExtrusionEntity::role_to_string(role));
        REQUIRE(command_is_active_for_role_region(
            pressure_advances, 0.8, segments, role, ModifierRegion::Modifier, bounds));
        saw_support |= is_support(role);
        saw_skirt |= role == erSkirt;
        saw_brim |= role == erBrim;
    }
    REQUIRE(saw_support);
    REQUIRE(saw_skirt);
    REQUIRE(saw_brim);
    REQUIRE(std::adjacent_find(
                pressure_advances.begin(), pressure_advances.end(),
                [](const GCodeCommand &lhs, const GCodeCommand &rhs) {
                    return almost_equal(lhs.value, 0.8, 0.01) &&
                           almost_equal(rhs.value, 0.2, 0.01);
                }) != pressure_advances.end());
}

TEST_CASE("Spiral vase processes filament modifier pressure advance markers",
          "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    config.set_deserialize_strict({
        { "gcode_flavor",            "marlin" },
        { "enable_pressure_advance", "1" },
        { "pressure_advance",        "0.2" },
        { "spiral_mode",             true },
        { "wall_loops",              1 },
        { "top_shell_layers",        0 },
        { "sparse_infill_density",   0 }
    });

    const std::string gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "modifier_pressure_advance", new ConfigOptionFloat(0.8) }});
    REQUIRE(gcode.find(";_FILAMENT_MODIFIER_PA_START") == std::string::npos);
    REQUIRE(gcode.find(";_FILAMENT_MODIFIER_PA_END") == std::string::npos);

    const std::vector<GCodeCommand> pressure_advances = gcode_commands(gcode, "M900", 'K');
    REQUIRE(std::adjacent_find(
                pressure_advances.begin(), pressure_advances.end(),
                [](const GCodeCommand &lhs, const GCodeCommand &rhs) {
                    return almost_equal(lhs.value, 0.8, 0.01) &&
                           almost_equal(rhs.value, 0.2, 0.01);
                }) != pressure_advances.end());
}

TEST_CASE("Filament modifier state is scoped inside path travel boundaries",
          "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    config.set_deserialize_strict({
        { "gcode_flavor",             "marlin" },
        { "enable_pressure_advance",  "1" },
        { "pressure_advance",         "0.2" },
        { "retraction_length",        "0.8" }
    });

    const std::string gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {
            { "modifier_nozzle_temperature", new ConfigOptionInt(230) },
            { "modifier_pressure_advance", new ConfigOptionFloat(0.8) }
        });
    const std::vector<ExtrusionSegment> segments = extrusion_segments(gcode);
    REQUIRE(!segments.empty());
    const double split = modifier_x_split(segments);
    const std::vector<GCodeCommand> temperatures = gcode_commands(gcode, "M104", 'S');
    const std::vector<GCodeCommand> pressure_advances = gcode_commands(gcode, "M900", 'K');
    const PathEvents events = path_events(gcode);

    bool verified_travel_path = false;
    for (const GCodeCommand &temperature_override : temperatures) {
        if (!almost_equal(temperature_override.value, 230., 0.01))
            continue;
        const auto first_extrusion = std::find_if(
            segments.begin(), segments.end(),
            [&temperature_override](const ExtrusionSegment &segment) {
                return segment.index > temperature_override.index;
            });
        if (first_extrusion == segments.end() ||
            modifier_region(*first_extrusion, split) != ModifierRegion::Modifier)
            continue;
        const auto previous_extrusion = std::find_if(
            segments.rbegin(), segments.rend(),
            [&temperature_override](const ExtrusionSegment &segment) {
                return segment.index < temperature_override.index;
            });
        if (previous_extrusion == segments.rend())
            continue;
        const auto travel = std::find_if(
            events.travels.rbegin(), events.travels.rend(),
            [&previous_extrusion, &temperature_override](size_t index) {
                return index > previous_extrusion->index && index < temperature_override.index;
            });
        if (travel == events.travels.rend())
            continue;
        const auto unretract = std::find_if(
            events.unretracts.rbegin(), events.unretracts.rend(),
            [&travel, &temperature_override](size_t index) {
                return index > *travel && index < temperature_override.index;
            });
        if (unretract == events.unretracts.rend())
            continue;
        const auto pressure_override = std::find_if(
            pressure_advances.begin(), pressure_advances.end(),
            [&travel, &first_extrusion](const GCodeCommand &command) {
                return almost_equal(command.value, 0.8, 0.01) &&
                       command.index > *travel &&
                       command.index < first_extrusion->index;
            });
        if (pressure_override == pressure_advances.end())
            continue;

        size_t next_path_boundary = std::string::npos;
        for (const size_t index : events.travels)
            if (index > first_extrusion->index)
                next_path_boundary = std::min(next_path_boundary, index);
        for (const size_t index : events.wipes)
            if (index > first_extrusion->index)
                next_path_boundary = std::min(next_path_boundary, index);
        if (next_path_boundary == std::string::npos)
            continue;

        const auto temperature_restore = std::find_if(
            temperatures.begin(), temperatures.end(),
            [&temperature_override, next_path_boundary](const GCodeCommand &command) {
                return almost_equal(command.value, 220., 0.01) &&
                       command.index > temperature_override.index &&
                       command.index < next_path_boundary;
            });
        const auto pressure_restore = std::find_if(
            pressure_advances.begin(), pressure_advances.end(),
            [&pressure_override, next_path_boundary](const GCodeCommand &command) {
                return almost_equal(command.value, 0.2, 0.01) &&
                       command.index > pressure_override->index &&
                       command.index < next_path_boundary;
            });
        if (temperature_restore == temperatures.end() || pressure_restore == pressure_advances.end())
            continue;

        REQUIRE(*travel < temperature_override.index);
        REQUIRE(*travel < pressure_override->index);
        REQUIRE(*unretract < temperature_override.index);
        REQUIRE(*unretract < pressure_override->index);
        REQUIRE(temperature_override.index < first_extrusion->index);
        REQUIRE(pressure_override->index < first_extrusion->index);
        REQUIRE(temperature_restore->index < next_path_boundary);
        REQUIRE(pressure_restore->index < next_path_boundary);
        verified_travel_path = true;
        break;
    }
    REQUIRE(verified_travel_path);
}

TEST_CASE("Filament modifier fan state is scoped after travel and role custom G-code",
          "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    configure_modifier_cooling(config, false);
    config.set_deserialize_strict({
        { "change_extrusion_role_gcode", "M117 FMD_CUSTOM" },
        { "retraction_length",           "0.8" }
    });

    const std::string gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "modifier_fan_speed", new ConfigOptionInt(5) }});
    const std::vector<ExtrusionSegment> segments = extrusion_segments(gcode);
    REQUIRE(!segments.empty());
    const double split = modifier_x_split(segments);
    const std::vector<GCodeCommand> fans = fan_commands(gcode, false);
    const std::vector<size_t> custom_commands = gcode_command_indices(gcode, "M117");
    const PathEvents events = path_events(gcode);

    bool verified_path = false;
    for (const GCodeCommand &override : fans) {
        if (!almost_equal(override.value, 12., 0.01))
            continue;
        const auto first_extrusion = std::find_if(
            segments.begin(), segments.end(),
            [&override](const ExtrusionSegment &segment) { return segment.index > override.index; });
        if (first_extrusion == segments.end() ||
            modifier_region(*first_extrusion, split) != ModifierRegion::Modifier)
            continue;
        const auto previous_extrusion = std::find_if(
            segments.rbegin(), segments.rend(),
            [&override](const ExtrusionSegment &segment) { return segment.index < override.index; });
        if (previous_extrusion == segments.rend())
            continue;
        const auto custom = std::find_if(
            custom_commands.rbegin(), custom_commands.rend(),
            [&previous_extrusion, &override](size_t index) {
                return index > previous_extrusion->index && index < override.index;
            });
        if (custom == custom_commands.rend())
            continue;
        const auto travel = std::find_if(
            events.travels.rbegin(), events.travels.rend(),
            [&previous_extrusion, &custom](size_t index) {
                return index > previous_extrusion->index && index < *custom;
            });
        if (travel == events.travels.rend())
            continue;

        size_t next_path_boundary = std::string::npos;
        for (const size_t index : events.travels)
            if (index > first_extrusion->index)
                next_path_boundary = std::min(next_path_boundary, index);
        for (const size_t index : events.wipes)
            if (index > first_extrusion->index)
                next_path_boundary = std::min(next_path_boundary, index);
        if (next_path_boundary == std::string::npos)
            continue;

        const auto restore = std::find_if(
            fans.begin(), fans.end(),
            [&override, next_path_boundary](const GCodeCommand &command) {
                return almost_equal(command.value, 51., 0.01) &&
                       command.index > override.index &&
                       command.index < next_path_boundary;
            });
        if (restore == fans.end())
            continue;

        REQUIRE(*travel < *custom);
        REQUIRE(*custom < override.index);
        REQUIRE(override.index < first_extrusion->index);
        REQUIRE(first_extrusion->index < restore->index);
        REQUIRE(restore->index < next_path_boundary);
        verified_path = true;
        break;
    }
    REQUIRE(verified_path);
}

TEST_CASE("Pressure advance fallback is isolated per tool",
          "[FilamentModifier][GCode][MultiTool]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    config.set_key_value("gcode_flavor", new ConfigOptionEnum<GCodeFlavor>(gcfMarlinFirmware));
    config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.4 }));
    config.set_key_value("printer_extruder_id", new ConfigOptionInts({ 1, 2 }));
    config.set_key_value(
        "printer_extruder_variant",
        new ConfigOptionStrings({ "Direct Drive Standard", "Direct Drive Standard" }));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({ 1.75, 1.75 }));
    config.set_key_value("filament_colour", new ConfigOptionStrings({ "#FF0000", "#00FF00" }));
    config.set_key_value("filament_type", new ConfigOptionStrings({ "PLA", "PLA" }));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map", new ConfigOptionInts({ 1, 2 }));
    config.set_key_value("default_filament_colour", new ConfigOptionStrings({ "#FF0000", "#00FF00" }));
    config.set_key_value("nozzle_temperature", new ConfigOptionInts({ 220, 220 }));
    config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts({ 220, 220 }));
    config.set_key_value("nozzle_temperature_range_low", new ConfigOptionInts({ 190, 190 }));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({ 240, 240 }));
    config.set_key_value("enable_pressure_advance", new ConfigOptionBools({ true, true }));
    config.set_key_value("pressure_advance", new ConfigOptionFloats({ 0.2, 0.4 }));
    config.set_key_value("flush_multiplier", new ConfigOptionFloats({ 1. }));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({ 0., 0., 0., 0. }));

    Model model;
    auto add_object = [&model](int extruder, double modifier_pressure_advance) {
        ModelObject *object = model.add_object();
        object->name = "filament_modifier_multi_tool.stl";
        add_cube_model_part(*object);
        ModelVolume *modifier = add_filament_modifier_cube(*object, 10.);
        modifier->config.set_key_value(
            "modifier_pressure_advance",
            new ConfigOptionFloat(modifier_pressure_advance));
        object->config.set_key_value("extruder", new ConfigOptionInt(extruder));
        object->add_instance();
    };
    add_object(1, 0.8);
    add_object(2, 0.9);

    Print print;
    prepare_print(print, model, config);
    const std::vector<ToolGCodeCommand> commands =
        pressure_advance_commands_by_tool(Slic3r::Test::gcode(print));

    auto require_transition = [&commands](int tool, double fallback, double override_value) {
        const auto override = std::find_if(
            commands.begin(), commands.end(),
            [tool, override_value](const ToolGCodeCommand &command) {
                return command.tool == tool && almost_equal(command.value, override_value, 0.01);
            });
        REQUIRE(override != commands.end());
        const auto previous = std::find_if(
            std::make_reverse_iterator(override), commands.rend(),
            [tool](const ToolGCodeCommand &command) { return command.tool == tool; });
        REQUIRE(previous != commands.rend());
        REQUIRE(almost_equal(previous->value, fallback, 0.01));
        const auto restore = std::find_if(
            override + 1, commands.end(),
            [tool](const ToolGCodeCommand &command) { return command.tool == tool; });
        REQUIRE(restore != commands.end());
        REQUIRE(almost_equal(restore->value, fallback, 0.01));
    };

    require_transition(0, 0.2, 0.8);
    require_transition(1, 0.4, 0.9);
    REQUIRE_FALSE(std::any_of(commands.begin(), commands.end(), [](const ToolGCodeCommand &command) {
        return command.tool == 1 &&
               (almost_equal(command.value, 0.2, 0.01) || almost_equal(command.value, 0.8, 0.01));
    }));
}

TEST_CASE("Filament modifier restores immediately preceding custom temperature pressure and fan state",
          "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    configure_modifier_cooling(config, true);
    config.set_deserialize_strict({
        { "gcode_flavor",                    "marlin" },
        { "enable_pressure_advance",         "1" },
        { "pressure_advance",                "0.2" },
        { "change_extrusion_role_gcode",     "M104 S215\nM900 K0.35\nM106 S204\nM106 P2 S153" }
    });

    const std::string gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {
            { "modifier_nozzle_temperature", new ConfigOptionInt(230) },
            { "modifier_pressure_advance", new ConfigOptionFloat(0.8) },
            { "modifier_fan_speed", new ConfigOptionInt(5) },
            { "modifier_aux_fan_speed", new ConfigOptionInt(5) }
        });
    const std::vector<ExtrusionSegment> segments = extrusion_segments(gcode);
    REQUIRE(!segments.empty());
    const double split = modifier_x_split(segments);
    const std::vector<GCodeCommand> temperatures = gcode_commands(gcode, "M104", 'S');
    const std::vector<GCodeCommand> pressure_advances = gcode_commands(gcode, "M900", 'K');
    const std::vector<GCodeCommand> main_fans = fan_commands(gcode, false);
    const std::vector<GCodeCommand> auxiliary_fans = fan_commands(gcode, true);

    const auto temperature_override = std::find_if(
        temperatures.begin(), temperatures.end(),
        [&segments, split](const GCodeCommand &command) {
            return almost_equal(command.value, 230., 0.01) &&
                   command_starts_region(command, segments, ModifierRegion::Modifier, split);
        });
    REQUIRE(temperature_override != temperatures.end());
    REQUIRE(temperature_override != temperatures.begin());
    REQUIRE(temperature_override + 1 != temperatures.end());
    REQUIRE(almost_equal((temperature_override - 1)->value, 215., 0.01));
    REQUIRE(almost_equal((temperature_override + 1)->value, 215., 0.01));

    const auto pressure_override = std::find_if(
        pressure_advances.begin(), pressure_advances.end(),
        [&segments, split](const GCodeCommand &command) {
            return almost_equal(command.value, 0.8, 0.01) &&
                   command_starts_region(command, segments, ModifierRegion::Modifier, split);
        });
    REQUIRE(pressure_override != pressure_advances.end());
    REQUIRE(pressure_override != pressure_advances.begin());
    REQUIRE(pressure_override + 1 != pressure_advances.end());
    REQUIRE(almost_equal((pressure_override - 1)->value, 0.35, 0.01));
    REQUIRE(almost_equal((pressure_override + 1)->value, 0.35, 0.01));

    auto require_fan_restore = [&segments, split](
                                  const std::vector<GCodeCommand> &commands,
                                  double custom_value) {
        const auto override = std::find_if(
            commands.begin(), commands.end(),
            [&segments, split](const GCodeCommand &command) {
                return almost_equal(command.value, 12., 0.01) &&
                       command_starts_region(command, segments, ModifierRegion::Modifier, split);
            });
        REQUIRE(override != commands.end());
        REQUIRE(override != commands.begin());
        REQUIRE(override + 1 != commands.end());
        REQUIRE(almost_equal((override - 1)->value, custom_value, 0.01));
        REQUIRE(almost_equal((override + 1)->value, custom_value, 0.01));
    };
    require_fan_restore(main_fans, 204.);
    require_fan_restore(auxiliary_fans, 153.);
}

TEST_CASE("Filament modifier restores a custom fan-off state",
          "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    configure_modifier_cooling(config, false);
    config.set_deserialize_strict({
        { "change_extrusion_role_gcode", "M107" }
    });

    const std::string gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "modifier_fan_speed", new ConfigOptionInt(5) }});
    const std::vector<ExtrusionSegment> segments = extrusion_segments(gcode);
    REQUIRE(!segments.empty());
    const double split = modifier_x_split(segments);
    const std::vector<GCodeCommand> fans = fan_state_commands(gcode, false);
    const auto override = std::find_if(
        fans.begin(), fans.end(),
        [&segments, split](const GCodeCommand &command) {
            return almost_equal(command.value, 12., 0.01) &&
                   command_starts_region(command, segments, ModifierRegion::Modifier, split);
        });
    REQUIRE(override != fans.end());
    REQUIRE(override != fans.begin());
    REQUIRE(override + 1 != fans.end());
    REQUIRE(almost_equal((override - 1)->value, 0., 0.01));
    REQUIRE(almost_equal((override + 1)->value, 0., 0.01));
}
