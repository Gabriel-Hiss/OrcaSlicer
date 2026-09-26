#pragma once

#include "libslic3r/Point.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace Slic3r {

// Ramp points are temperature x seconds at full power, relative to one origin.
// Settle points add the time Klipper needs after crossing each target before M109/M190 returns.
class HeaterCurve
{
public:
    HeaterCurve() = default;
    HeaterCurve(std::vector<Vec2d> ramp, std::vector<Vec2d> settle);

    bool valid() const { return m_ramp.size() >= 2; }

    // Extrapolates past either end of the ramp.
    double ramp_time(double temperature) const;
    double ramp_temperature(double seconds) const;
    // Holds the nearest measured value outside the settle range.
    double settle_time(double target) const;
    // Klipper TEMPERATURE_WAIT crosses the target without settling.
    double heat_time(double from, double to, bool settle = true) const;

private:
    std::vector<Vec2d> m_ramp;
    std::vector<Vec2d> m_settle;
};

struct HeatingEvent
{
    enum class Heater : unsigned char { Nozzle, Bed };

    Heater                heater{Heater::Nozzle};
    unsigned int          index{0};      // nozzle extruder index
    std::optional<double> target;
    std::optional<double> wait_for;
    bool                  settle{true};
    size_t                move_id{0};    // moves stored before this command
    double                machine_time{0.}; // motion seconds before this command
};

// Heater times may overlap; wait counts only blocked time.
struct HeatingTimes
{
    double nozzle{0.};
    double bed{0.};
    double wait{0.};
};

// Cooling is not modelled. Heaters without a valid curve are ignored.
HeatingTimes estimate_heating_times(const std::vector<HeatingEvent> &events,
                                    const HeaterCurve               &nozzle,
                                    const HeaterCurve               &bed,
                                    double                           ambient,
                                    double                           first_extrusion_time);

// Sample time is seconds since setting the target.
struct HeatingSample
{
    double time{0.};
    double temperature{0.};
    double power{0.}; // PWM, 0 .. max_power
};

struct HeatingRun
{
    double                     target{0.};
    double                     release{0.}; // seconds until M109/M190 returns
    std::vector<HeatingSample> samples;
};

struct HeaterCurvePoints
{
    std::vector<Vec2d> ramp;
    std::vector<Vec2d> settle;
};

// Averages full-power heating over `step` °C bands (power >= 95% of `max_power`).
// Settle is the average delay beyond the ramp prediction at each target.
HeaterCurvePoints fit_heater_curve(const std::vector<HeatingRun> &runs, double max_power, double step = 5.);

} // namespace Slic3r
