#include "HeatingTime.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace Slic3r {

namespace {

constexpr double EPSILON_TEMPERATURE = 0.01;

// Extrapolate the end segments when x lies outside the measured points.
double interpolate(const std::vector<Vec2d> &points, double x, int x_axis)
{
    const int y_axis = 1 - x_axis;
    auto      it     = std::lower_bound(points.begin(), points.end(), x,
                                        [x_axis](const Vec2d &p, double value) { return p[x_axis] < value; });
    if (it == points.begin())
        ++it;
    else if (it == points.end())
        --it;
    const Vec2d &a = *(it - 1);
    const Vec2d &b = *it;
    return a[y_axis] + (x - a[x_axis]) * (b[y_axis] - a[y_axis]) / (b[x_axis] - a[x_axis]);
}

struct HeaterSim
{
    const HeaterCurve *curve{nullptr};
    double             temperature{0.}; // at `since` while heating, current otherwise
    double             target{0.};
    bool               heating{false};
    double             since{0.};
    double             done_at{0.};
    double             heating_time{0.};

    void finish_if_done(double clock)
    {
        if (heating && clock >= done_at) {
            heating_time += done_at - since;
            temperature = target;
            heating     = false;
        }
    }

    void set_target(double new_target, double clock)
    {
        finish_if_done(clock);
        if (std::abs(new_target - target) < EPSILON_TEMPERATURE)
            return;
        if (heating) {
            const double reached = curve->ramp_temperature(curve->ramp_time(temperature) + clock - since);
            heating_time += clock - since;
            temperature = std::min(reached, target);
            heating     = false;
        }
        target = new_target;
        if (new_target > temperature + EPSILON_TEMPERATURE) {
            heating = true;
            since   = clock;
            done_at = clock + curve->heat_time(temperature, new_target);
        }
    }

    double release_at(double wait_for, bool settle, double clock)
    {
        finish_if_done(clock);
        if (!heating || wait_for > target + EPSILON_TEMPERATURE)
            return clock;
        if (settle && wait_for >= target - EPSILON_TEMPERATURE)
            return done_at;
        return std::max(clock, since + curve->heat_time(temperature, wait_for, false));
    }

    double total_heating_time(double cutoff) const { return heating_time + (heating ? std::max(0., std::min(done_at, cutoff) - since) : 0.); }
};

} // namespace

HeaterCurve::HeaterCurve(std::vector<Vec2d> ramp, std::vector<Vec2d> settle)
{
    auto by_temperature = [](const Vec2d &a, const Vec2d &b) { return a.x() < b.x(); };
    std::sort(ramp.begin(), ramp.end(), by_temperature);
    for (const Vec2d &p : ramp)
        if (std::isfinite(p.x()) && std::isfinite(p.y()) &&
            (m_ramp.empty() || (p.x() > m_ramp.back().x() && p.y() > m_ramp.back().y())))
            m_ramp.push_back(p);

    std::sort(settle.begin(), settle.end(), by_temperature);
    for (const Vec2d &p : settle)
        if (std::isfinite(p.x()) && std::isfinite(p.y()) &&
            (m_settle.empty() || p.x() > m_settle.back().x()))
            m_settle.emplace_back(p.x(), std::max(0., p.y()));
}

double HeaterCurve::ramp_time(double temperature) const { return interpolate(m_ramp, temperature, 0); }

double HeaterCurve::ramp_temperature(double seconds) const { return interpolate(m_ramp, seconds, 1); }

double HeaterCurve::settle_time(double target) const
{
    if (m_settle.empty())
        return 0.;
    if (m_settle.size() == 1 || target <= m_settle.front().x())
        return m_settle.front().y();
    if (target >= m_settle.back().x())
        return m_settle.back().y();
    return interpolate(m_settle, target, 0);
}

double HeaterCurve::heat_time(double from, double to, bool settle) const
{
    if (!valid() || to <= from)
        return 0.;
    return ramp_time(to) - ramp_time(from) + (settle ? settle_time(to) : 0.);
}

HeatingTimes estimate_heating_times(const std::vector<HeatingEvent> &events,
                                    const HeaterCurve               &nozzle,
                                    const HeaterCurve               &bed,
                                    double                           ambient,
                                    double                           first_extrusion_time)
{
    std::vector<HeaterSim> nozzles;
    HeaterSim              bed_sim{&bed, ambient};
    double                 blocked = 0.;

    for (const HeatingEvent &event : events) {
        const bool is_bed = event.heater == HeatingEvent::Heater::Bed;
        if (!(is_bed ? bed : nozzle).valid())
            continue;
        if (!is_bed && event.index >= nozzles.size())
            nozzles.resize(event.index + 1, HeaterSim{&nozzle, ambient});
        HeaterSim &sim = is_bed ? bed_sim : nozzles[event.index];

        const double clock = event.machine_time + blocked;
        if (event.target)
            sim.set_target(*event.target, clock);
        if (event.wait_for)
            blocked += sim.release_at(*event.wait_for, event.settle, clock) - clock;
    }

    HeatingTimes times;
    const double cutoff = first_extrusion_time + blocked;
    for (const HeaterSim &sim : nozzles)
        times.nozzle += sim.total_heating_time(cutoff);
    times.bed  = bed_sim.total_heating_time(cutoff);
    times.wait = blocked;
    return times;
}

HeaterCurvePoints fit_heater_curve(const std::vector<HeatingRun> &runs, double max_power, double step)
{
    const double full_power = 0.95 * max_power;

    // Each band records total seconds and number of full-power crossings.
    std::map<long, std::pair<double, int>> bands;
    for (const HeatingRun &run : runs) {
        const std::vector<HeatingSample> &samples = run.samples;
        long   last_edge = 0;
        double last_edge_time = 0.;
        bool   has_edge = false;
        for (size_t i = 1; i < samples.size(); ++i) {
            const HeatingSample &a = samples[i - 1];
            const HeatingSample &b = samples[i];
            if (a.power < full_power || b.power < full_power) {
                has_edge = false;
                continue;
            }
            if (b.temperature <= a.temperature)
                continue;
            for (long edge = long(std::floor(a.temperature / step)) + 1; edge * step <= b.temperature; ++edge) {
                const double t = a.time + (edge * step - a.temperature) * (b.time - a.time) / (b.temperature - a.temperature);
                if (has_edge && last_edge == edge - 1) {
                    std::pair<double, int> &band = bands[edge - 1];
                    band.first += t - last_edge_time;
                    ++band.second;
                }
                last_edge      = edge;
                last_edge_time = t;
                has_edge       = true;
            }
        }
    }

    HeaterCurvePoints points;
    for (const auto &[band, sum_count] : bands) {
        if (points.ramp.empty())
            points.ramp.emplace_back(band * step, 0.);
        else if (std::abs(points.ramp.back().x() - band * step) > 1e-6)
            break; // only fit consecutive bands from the coldest crossing
        points.ramp.emplace_back((band + 1) * step, points.ramp.back().y() + sum_count.first / sum_count.second);
    }

    const HeaterCurve ramp(points.ramp, {});
    if (!ramp.valid())
        return {};
    std::map<double, std::pair<double, int>> settles;
    for (const HeatingRun &run : runs) {
        if (run.samples.empty() || run.samples.front().temperature >= run.target)
            continue;
        std::pair<double, int> &settle = settles[run.target];
        settle.first += run.release - run.samples.front().time - ramp.heat_time(run.samples.front().temperature, run.target, false);
        ++settle.second;
    }
    for (const auto &[target, sum_count] : settles)
        points.settle.emplace_back(target, std::max(0., sum_count.first / sum_count.second));
    return points;
}

} // namespace Slic3r
