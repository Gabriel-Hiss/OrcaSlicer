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
    arrange_objects(model, InfiniteBed{}, ArrangeParams{ scaled(min_object_distance(config)) });
    for (ModelObject *model_object : model.objects) {
        model_object->ensure_on_bed();
        print.auto_assign_extruders(model_object);
    }

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
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
    constexpr std::string_view type = "TYPE:";
    const size_t type_pos = comment.find(type);
    if (type_pos != std::string_view::npos)
        role = ExtrusionEntity::string_to_role(comment.substr(type_pos + type.size()));
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

bool continues_collinearly(const GeometrySegment &lhs, const GeometrySegment &rhs)
{
    if (lhs.role != rhs.role || !almost_equal(lhs.z, rhs.z) ||
        !almost_equal(lhs.x1, rhs.x0) || !almost_equal(lhs.y1, rhs.y0))
        return false;

    const double lhs_x = lhs.x1 - lhs.x0;
    const double lhs_y = lhs.y1 - lhs.y0;
    const double rhs_x = rhs.x1 - rhs.x0;
    const double rhs_y = rhs.y1 - rhs.y0;
    return std::abs(lhs_x * rhs_y - lhs_y * rhs_x) <= 1e-4 &&
           lhs_x * rhs_x + lhs_y * rhs_y > 0.;
}

std::vector<GeometrySegment> normalized_geometry(const std::vector<ExtrusionSegment> &segments)
{
    std::vector<GeometrySegment> normalized;
    for (const ExtrusionSegment &segment : segments) {
        const GeometrySegment geometry {
            segment.x0, segment.y0, segment.x1, segment.y1, segment.z, segment.role
        };
        if (!normalized.empty() && continues_collinearly(normalized.back(), geometry)) {
            normalized.back().x1 = geometry.x1;
            normalized.back().y1 = geometry.y1;
        } else {
            normalized.push_back(geometry);
        }
    }
    return normalized;
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
        { "additional_cooling_fan_speed",  "30" }
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
    constexpr double margin = 0.15;
    const double min_x = std::min(segment.x0, segment.x1);
    const double max_x = std::max(segment.x0, segment.x1);
    if (min_x >= bounds.min_x - margin && max_x <= bounds.max_x + margin)
        return ModifierRegion::Modifier;
    if (max_x <= bounds.min_x + margin || min_x >= bounds.max_x - margin)
        return ModifierRegion::Normal;
    return ModifierRegion::Boundary;
}

constexpr std::array<ExtrusionRole, 6> object_extrusion_roles {
    erPerimeter,
    erExternalPerimeter,
    erInternalInfill,
    erSolidInfill,
    erTopSolidInfill,
    erBottomSurface
};

void configure_multi_role_infill(DynamicPrintConfig &config)
{
    config.set_deserialize_strict({
        { "wall_loops",             2 },
        { "sparse_infill_density",  15 },
        { "top_shell_layers",       2 },
        { "bottom_shell_layers",    2 },
        { "extra_solid_infills",    "10" }
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

bool has_boundary_segment(const std::vector<ExtrusionSegment> &segments, const ModifierBounds &bounds)
{
    return std::any_of(segments.begin(), segments.end(), [&bounds](const ExtrusionSegment &segment) {
        return modifier_region(segment, bounds) == ModifierRegion::Boundary;
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
        for (const ExtrusionSegment &segment : segments) {
            if (segment.index > command.index)
                return segment.role == role && modifier_region(segment, bounds) == region;
        }
    }
    return false;
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

TEST_CASE("Filament modifier scopes volumetric feedrate on a partial model", "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();

    const std::string uncapped_gcode = slice_cube_with_optional_filament_modifier(
        config,
        10.,
        {{ "modifier_max_volumetric_speed", new ConfigOptionFloat(0.) }});
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
               command_starts_region(command, segments, ModifierRegion::Normal, split);
    });
    REQUIRE(restore != temperatures.end());
    REQUIRE(command_starts_region(*override, segments, ModifierRegion::Modifier, split));
    REQUIRE(command_starts_region(*restore, segments, ModifierRegion::Normal, split));

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

    const std::string base_gcode = slice_cube_with_optional_filament_modifier(config, 10.);
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
               command_starts_region(command, segments, ModifierRegion::Normal, split);
    });
    REQUIRE(restore != pressure_advances.end());
    REQUIRE(command_starts_region(*override, segments, ModifierRegion::Modifier, split));
    REQUIRE(command_starts_region(*restore, segments, ModifierRegion::Normal, split));

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
        {{ "modifier_fan_speed", new ConfigOptionInt(80) }});
    const std::vector<ExtrusionSegment> segments = extrusion_segments(modified_gcode);
    REQUIRE(!segments.empty());
    const double split = modifier_x_split(segments);

    const std::vector<GCodeCommand> main_fans = fan_commands(modified_gcode, false);
    const auto override = std::find_if(main_fans.begin(), main_fans.end(), [](const GCodeCommand &command) {
        return std::abs(command.value - 204.) < 0.01;
    });
    REQUIRE(override != main_fans.end());
    const auto restore = std::find_if(override + 1, main_fans.end(), [&segments, split](const GCodeCommand &command) {
        return std::abs(command.value - 51.) < 0.01 &&
               command_starts_region(command, segments, ModifierRegion::Normal, split);
    });
    REQUIRE(restore != main_fans.end());
    REQUIRE(command_starts_region(*override, segments, ModifierRegion::Modifier, split));
    REQUIRE(command_starts_region(*restore, segments, ModifierRegion::Normal, split));

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
        {{ "modifier_aux_fan_speed", new ConfigOptionInt(70) }});
    const std::vector<ExtrusionSegment> segments = extrusion_segments(modified_gcode);
    REQUIRE(!segments.empty());
    const double split = modifier_x_split(segments);

    const std::vector<GCodeCommand> auxiliary_fans = fan_commands(modified_gcode, true);
    const auto override = std::find_if(auxiliary_fans.begin(), auxiliary_fans.end(), [](const GCodeCommand &command) {
        return std::abs(command.value - 178.) < 0.01;
    });
    REQUIRE(override != auxiliary_fans.end());
    const auto restore = std::find_if(override + 1, auxiliary_fans.end(), [&segments, split](const GCodeCommand &command) {
        return std::abs(command.value - 76.) < 0.01 &&
               command_starts_region(command, segments, ModifierRegion::Normal, split);
    });
    REQUIRE(restore != auxiliary_fans.end());
    REQUIRE(command_starts_region(*override, segments, ModifierRegion::Modifier, split));
    REQUIRE(command_starts_region(*restore, segments, ModifierRegion::Normal, split));

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
            {{ "modifier_aux_fan_speed", new ConfigOptionInt(70) }}),
        true);
    REQUIRE(disabled_auxiliary_fans.empty());
}

TEST_CASE("Filament modifier control without a volume is neutral", "[FilamentModifier][GCode]")
{
    DynamicPrintConfig config = filament_modifier_test_config();
    configure_modifier_cooling(config, true);
    config.set_deserialize_strict({
        { "gcode_flavor",             "marlin" },
        { "enable_pressure_advance",  "1" },
        { "pressure_advance",         "0.2" }
    });

    const std::string gcode = slice_cube_with_optional_filament_modifier(config);
    REQUIRE_FALSE(has_command_value(gcode_commands(gcode, "M104", 'S'), 230.));
    REQUIRE_FALSE(has_command_value(gcode_commands(gcode, "M900", 'K'), 0.8));
    REQUIRE_FALSE(has_command_value(fan_commands(gcode, false), 204.));
    REQUIRE_FALSE(has_command_value(fan_commands(gcode, true), 178.));
}

TEST_CASE("Filament modifier only subdivides collinear paths at volume boundaries", "[FilamentModifier][GCode]")
{
    constexpr double modifier_width = 10.;
    DynamicPrintConfig config = filament_modifier_test_config();
    configure_multi_role_infill(config);

    const std::string baseline_gcode = slice_cube_with_optional_filament_modifier(config);
    const std::string modified_gcode = slice_cube_with_optional_filament_modifier(
        config,
        modifier_width,
        {{ "print_flow_ratio", new ConfigOptionFloat(1.5) }});
    const std::vector<ExtrusionSegment> baseline = extrusion_segments(baseline_gcode);
    const std::vector<ExtrusionSegment> modified = extrusion_segments(modified_gcode);
    REQUIRE(!baseline.empty());
    REQUIRE(!modified.empty());

    const ModifierBounds baseline_bounds = modifier_bounds(baseline, modifier_width);
    const ModifierBounds modified_bounds = modifier_bounds(modified, modifier_width);
    REQUIRE(has_boundary_segment(baseline, baseline_bounds));
    REQUIRE_FALSE(has_boundary_segment(modified, modified_bounds));
    REQUIRE_FALSE(std::any_of(modified.begin(), modified.end(), [&modified_bounds](const ExtrusionSegment &segment) {
        return segment.role == erPerimeter && modifier_region(segment, modified_bounds) == ModifierRegion::Boundary;
    }));

    REQUIRE(same_geometry(normalized_geometry(baseline), normalized_geometry(modified)));
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
        slice_cube_with_optional_filament_modifier(config));
    const std::vector<ExtrusionSegment> flow_modified = extrusion_segments(
        slice_cube_with_optional_filament_modifier(
            config,
            modifier_width,
            {{ "print_flow_ratio", new ConfigOptionFloat(1.5) }}));
    const std::vector<ExtrusionSegment> speed_modified = extrusion_segments(
        slice_cube_with_optional_filament_modifier(
            config,
            modifier_width,
            {{ "modifier_max_volumetric_speed", new ConfigOptionFloat(0.6) }}));
    REQUIRE(!baseline.empty());
    REQUIRE(!flow_modified.empty());
    REQUIRE(!speed_modified.empty());

    const ModifierBounds baseline_bounds = modifier_bounds(baseline, modifier_width);
    const ModifierBounds flow_bounds = modifier_bounds(flow_modified, modifier_width);
    const ModifierBounds speed_bounds = modifier_bounds(speed_modified, modifier_width);

    for (const ExtrusionRole role : object_extrusion_roles) {
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

    for (const ExtrusionRole role : object_extrusion_roles) {
        INFO("Role: " << ExtrusionEntity::role_to_string(role));
        REQUIRE(has_role_in_region(segments, role, ModifierRegion::Modifier, bounds));
        REQUIRE(has_role_in_region(segments, role, ModifierRegion::Normal, bounds));
        REQUIRE(command_starts_role_region(
            temperatures, 230., segments, role, ModifierRegion::Modifier, bounds));
        REQUIRE(command_starts_role_region(
            temperatures, 220., segments, role, ModifierRegion::Normal, bounds));
    }
}
