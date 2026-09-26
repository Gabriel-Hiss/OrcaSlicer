#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/HeatingTime.hpp"

#include <cmath>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

HeaterCurve linear_curve(double rate) { return HeaterCurve({{0., 0.}, {300., 300. / rate}}, {}); }

HeatingEvent set_target(HeatingEvent::Heater heater, double target, double machine_time)
{
    HeatingEvent event;
    event.heater       = heater;
    event.target       = target;
    event.machine_time = machine_time;
    return event;
}

HeatingEvent wait_for(HeatingEvent::Heater heater, double target, double machine_time)
{
    HeatingEvent event = set_target(heater, target, machine_time);
    event.wait_for     = target;
    return event;
}

} // namespace

TEST_CASE("Heat time is the ramp difference plus the settle time of the target", "[HeatingTime]")
{
    const HeaterCurve curve({{20., 0.}, {120., 50.}, {220., 150.}}, {{100., 10.}, {200., 30.}});
    CHECK_THAT(curve.heat_time(60., 170.), WithinAbs(80. + 24., 1e-9));
    CHECK_THAT(curve.heat_time(60., 170., false), WithinAbs(80., 1e-9));
    CHECK_THAT(curve.heat_time(20., 250., true) - curve.heat_time(20., 250., false), WithinAbs(30., 1e-9));
    CHECK_THAT(curve.heat_time(10., 20., false), WithinAbs(5., 1e-9));
    CHECK(curve.heat_time(170., 60.) == 0.);
}

TEST_CASE("Heaters heating in parallel block only while the printer waits on them", "[HeatingTime]")
{
    using H = HeatingEvent::Heater;
    const std::vector<HeatingEvent> events = {set_target(H::Nozzle, 150., 0.), set_target(H::Bed, 60., 0.),
                                              wait_for(H::Bed, 60., 5.), wait_for(H::Nozzle, 220., 10.)};
    const HeatingTimes times = estimate_heating_times(events, linear_curve(2.), linear_curve(0.1), 20., 10.);

    CHECK_THAT(times.bed, WithinAbs(400., 1e-9));
    CHECK_THAT(times.nozzle, WithinAbs(65. + 35., 1e-9));
    CHECK_THAT(times.wait, WithinAbs(395. + 35., 1e-9));
}

TEST_CASE("A heater retargeted before reaching its target keeps heating from where it got", "[HeatingTime]")
{
    using H = HeatingEvent::Heater;
    const std::vector<HeatingEvent> events = {set_target(H::Nozzle, 150., 0.), wait_for(H::Nozzle, 220., 20.)};
    const HeatingTimes times = estimate_heating_times(events, linear_curve(2.), linear_curve(0.1), 20., 20.);

    CHECK_THAT(times.nozzle, WithinAbs(100., 1e-9));
    CHECK_THAT(times.wait, WithinAbs(80., 1e-9));
    CHECK(times.bed == 0.);
}

TEST_CASE("Waits on an uncalibrated heater add nothing", "[HeatingTime]")
{
    using H = HeatingEvent::Heater;
    const std::vector<HeatingEvent> events = {wait_for(H::Bed, 60., 0.), wait_for(H::Nozzle, 220., 0.)};
    const HeatingTimes times = estimate_heating_times(events, linear_curve(2.), HeaterCurve(), 20., 0.);

    CHECK(times.bed == 0.);
    CHECK_THAT(times.nozzle, WithinAbs(100., 1e-9));
    CHECK_THAT(times.wait, WithinAbs(100., 1e-9));
}

TEST_CASE("Heating stops accumulating when the first extrusion begins", "[HeatingTime]")
{
    using H = HeatingEvent::Heater;
    const std::vector<HeatingEvent> events = {set_target(H::Nozzle, 220., 0.), set_target(H::Bed, 80., 0.),
                                              wait_for(H::Nozzle, 220., 0.)};
    const HeatingTimes times = estimate_heating_times(events, linear_curve(2.), linear_curve(0.1), 20., 5.);

    CHECK_THAT(times.nozzle, WithinAbs(100., 1e-9));
    CHECK_THAT(times.bed, WithinAbs(105., 1e-9));
    CHECK_THAT(times.wait, WithinAbs(100., 1e-9));
}

TEST_CASE("Fitting recovers the full-power rate and the settle time of a heater", "[HeatingTime]")
{
    // Reduced power near the target adds 15 s beyond the full-power ramp; release adds 4 s.
    auto make_run = [](double start, double target) {
        HeatingRun run;
        run.target = target;
        const double knee = target - 10.;
        const double t_knee = (knee - start) / 2.;
        const double t_target = t_knee + 20.;
        for (double t = 0.; t <= t_target + 10.; t += 0.25) {
            const double temperature = t < t_knee ? start + 2. * t : std::min(target, knee + 0.5 * (t - t_knee));
            run.samples.push_back({t, temperature, t < t_knee ? 1. : 0.4});
        }
        run.release = t_target + 4.;
        return run;
    };
    std::vector<HeatingRun> runs = {make_run(30., 100.), make_run(30., 200.), make_run(32., 200.)};
    for (HeatingRun &run : runs) {
        run.release += 5.;
        for (HeatingSample &sample : run.samples)
            sample.time += 5.;
    }
    const HeaterCurvePoints points = fit_heater_curve(runs, 1.);
    const HeaterCurve curve(points.ramp, points.settle);
    REQUIRE(curve.valid());

    CHECK_THAT(curve.ramp_time(180.) - curve.ramp_time(40.), WithinAbs(70., 0.1));
    REQUIRE(points.settle.size() == 2);
    CHECK_THAT(curve.heat_time(30., 100.), WithinAbs(35. + 15. + 4., 0.2));
    CHECK_THAT(curve.heat_time(30., 200.), WithinAbs(85. + 15. + 4., 0.2));
}
