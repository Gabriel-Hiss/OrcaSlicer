#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include "libslic3r/calib.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

// The width-resolution getters are protected; expose them so the resolution can be asserted directly.
struct PaPatternProbe : public CalibPressureAdvancePattern
{
    using CalibPressureAdvancePattern::CalibPressureAdvancePattern;
    using CalibPressureAdvancePattern::line_width;
    using CalibPressureAdvancePattern::line_width_first_layer;
};

void require_flow_values(const std::vector<double> &actual, const std::vector<double> &expected)
{
    REQUIRE(actual.size() == expected.size());

    for (size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        REQUIRE(actual[i] == Catch::Approx(expected[i]).margin(1e-12));
    }
}

} // namespace

TEST_CASE("Zero calibration line width resolves to a positive default", "[Calib][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"line_width", "0"},
        {"initial_layer_line_width", "0"},
    });

    Model model;
    model.add_object("cube", "", make_cube(20, 20, 20))->add_instance();

    Calib_Params params;
    params.mode = CalibMode::Calib_PA_Pattern;

    PaPatternProbe pattern(params, config, /* is_bbl_machine */ true, *model.objects.front(), Vec3d(0, 0, 0));

    REQUIRE(pattern.line_width() > 0.);
    REQUIRE(pattern.line_width_first_layer() > 0.);
}

TEST_CASE("Custom flowrate values include zero and sorted stepped offsets", "[Calib][FlowRate]")
{
    SECTION("asymmetric user range includes on-grid negative bound and excludes off-grid positive bound")
    {
        require_flow_values(flowrate_custom_values({-0.1, 0.03, 0.05, 0.025}), {-0.1, -0.05, 0., 0.025});
    }

    SECTION("perfectionist preset range matches the existing fine plate value grid")
    {
        require_flow_values(flowrate_custom_values({-0.04, 0.035, 0.005, 0.005}),
                            {-0.04, -0.035, -0.03, -0.025, -0.02, -0.015, -0.01, -0.005,
                             0., 0.005, 0.01, 0.015, 0.02, 0.025, 0.03, 0.035});
    }

    SECTION("zero lower bound emits no negative side")
    {
        require_flow_values(flowrate_custom_values({0., 0.02, 0.01, 0.01}), {0., 0.01, 0.02});
    }
}

TEST_CASE("Custom flowrate calibration blocks are named pads with engraved labels", "[Calib][FlowRate]")
{
    const std::string font_path = std::string(TEST_DATA_DIR) + "/../../resources/fonts/HarmonyOS_Sans_SC_Regular.ttf";

    Model model;
    REQUIRE(add_flowrate_calib_blocks(model, {-0.05, 0., 0.025}, font_path));

    REQUIRE(model.objects.size() == 3);
    REQUIRE(model.objects[0]->name == "flowrate_m0.05");
    REQUIRE(model.objects[1]->name == "flowrate_0");
    REQUIRE(model.objects[2]->name == "flowrate_0.025");

    for (const ModelObject *object : model.objects) {
        REQUIRE(object->volumes.size() == 3);

        size_t             negative_volume_count = 0;
        const ModelVolume *label_volume          = nullptr;
        BoundingBoxf3      model_part_bbox;

        for (const ModelVolume *volume : object->volumes) {
            if (volume->is_negative_volume()) {
                ++negative_volume_count;
                label_volume = volume;
            } else if (volume->is_model_part()) {
                model_part_bbox.merge(volume->mesh().bounding_box());
            }
        }

        REQUIRE(negative_volume_count == 1);
        REQUIRE(label_volume != nullptr);
        REQUIRE(label_volume->name == "label");
        REQUIRE_FALSE(label_volume->mesh().empty());

        const BoundingBoxf3 label_bbox = label_volume->mesh().bounding_box();
        CHECK(label_bbox.min.x() >= -11.5 - 1e-3);
        CHECK(label_bbox.max.x() <= 11.5 + 1e-3);
        CHECK(label_bbox.min.y() >= 4.5 - 1e-3);
        CHECK(label_bbox.max.y() <= 15. + 1e-3);
        CHECK(label_bbox.min.z() >= 0.2 - 1e-3);
        CHECK(label_bbox.max.z() <= 1. + 1e-3);

        const Vec3d model_part_size = model_part_bbox.size();
        CHECK(model_part_size.x() == Catch::Approx(30.).margin(1e-3));
        CHECK(model_part_size.y() == Catch::Approx(30.).margin(1e-3));
        CHECK(model_part_size.z() == Catch::Approx(2.).margin(1e-3));
    }

    Model invalid_font_model;
    REQUIRE_FALSE(add_flowrate_calib_blocks(invalid_font_model, {-0.05, 0., 0.025}, font_path + ".missing"));
}
