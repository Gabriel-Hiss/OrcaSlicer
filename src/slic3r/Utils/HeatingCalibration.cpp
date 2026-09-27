#include "HeatingCalibration.hpp"

#include "Http.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <thread>

using json = nlohmann::json;

namespace Slic3r {

namespace {

// Park 3 mm above the bed so the part fan blows on it and on the bed sensor.
constexpr double park_z = 3.;
constexpr double park_margin = 10.; // mm inside the reachable area
constexpr int    travel_feedrate = 6000;
constexpr long   heating_timeout = 3600;
constexpr long   cleanup_timeout = 30;
constexpr double cooling_stall_time = 180.;
constexpr double cooling_stall_drop = 0.5;
constexpr int    calibration_idle_timeout = 24 * 3600;

std::string fmt_temp(double temperature) { return float_to_string_decimal_point(double(std::lround(temperature)), 0); }

} // namespace

HeatingCalibration::HeatingCalibration(Params params, StatusFn on_status)
    : m_params(std::move(params)), m_on_status(std::move(on_status))
{}

std::vector<std::pair<double, double>> HeatingCalibration::cycle_targets(const Params &params)
{
    std::vector<std::pair<double, double>> targets;
    for (int k = 1; k <= params.targets; ++k) {
        const double nozzle = std::round(params.nozzle_min + (params.nozzle_max - params.nozzle_min) * k / params.targets);
        const double bed    = std::round(params.bed_min + (params.bed_max - params.bed_min) * k / params.targets);
        if (targets.empty() || (targets.back().first != nozzle && targets.back().second != bed))
            targets.emplace_back(nozzle, bed);
    }
    return targets;
}

std::string HeatingCalibration::base_url() const { return (m_params.use_https ? "https://" : "http://") + m_params.host + ":" + std::to_string(m_params.port); }

std::string HeatingCalibration::get(const std::string &path) const
{
    std::string body, error;
    auto        http = Http::get(base_url() + path);
    if (m_params.use_https)
        http.tls_verify(true);
    if (!m_params.api_key.empty())
        http.header("X-Api-Key", m_params.api_key);
    http.on_progress([this](Http::Progress, bool &cancel) { cancel = m_cancelled; });
    http.timeout_connect(5)
        .timeout_max(30)
        .on_complete([&](std::string response, unsigned) { body = std::move(response); })
        .on_error([&](std::string response, std::string message, unsigned status) {
            error = message.empty() ? "HTTP " + std::to_string(status) + " " + response : message;
        })
        .perform_sync();
    check_cancelled();
    if (!error.empty())
        throw RuntimeError(format(_u8L("Moonraker request %1% failed: %2%"), path, error));
    if (body.empty())
        throw RuntimeError(_u8L("Moonraker returned an empty response."));
    return body;
}

void HeatingCalibration::run_gcode(const std::string &script, long timeout, bool cancellable) const
{
    std::string error;
    bool completed = false;
    auto        http = Http::post(base_url() + "/printer/gcode/script");
    if (m_params.use_https)
        http.tls_verify(true);
    http.header("Content-Type", "application/json").set_post_body(json{{"script", script}}.dump());
    if (!m_params.api_key.empty())
        http.header("X-Api-Key", m_params.api_key);
    // Klipper finishes a submitted script even if its HTTP request is cancelled.
    if (cancellable)
        http.on_progress([this](Http::Progress, bool &cancel) { cancel = m_cancelled; });
    http.timeout_connect(5)
        .timeout_max(timeout)
        .on_complete([&](std::string, unsigned) { completed = true; })
        .on_error([&](std::string response, std::string message, unsigned status) {
            error = message;
            if (auto parsed = json::parse(response, nullptr, false); parsed.is_object() && parsed.contains("error"))
                error = parsed["error"].value("message", response);
            else if (error.empty())
                error = "HTTP " + std::to_string(status);
        })
        .perform_sync();
    if (!completed && error.empty())
        error = "Moonraker did not complete the request.";
    if (!error.empty())
        throw RuntimeError(format(_u8L("Printer rejected \"%1%\": %2%"), script, error));
    if (cancellable)
        check_cancelled();
}

HeatingCalibration::Reading HeatingCalibration::read_heaters() const
{
    const json  result = json::parse(get("/printer/objects/query?extruder=temperature,power&heater_bed=temperature,power"))["result"];
    const json &status = result["status"];
    Reading     reading;
    reading.eventtime    = result.value("eventtime", 0.);
    reading.nozzle       = status["extruder"].value("temperature", 0.);
    reading.nozzle_power = status["extruder"].value("power", 0.);
    reading.bed          = status["heater_bed"].value("temperature", 0.);
    reading.bed_power    = status["heater_bed"].value("power", 0.);
    return reading;
}

void HeatingCalibration::check_cancelled() const
{
    if (m_cancelled)
        throw RuntimeError(_u8L("Calibration cancelled."));
}

void HeatingCalibration::sleep(double seconds) const
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        check_cancelled();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check_cancelled();
}

void HeatingCalibration::report(const std::string &message, double nozzle_target, double bed_target, const Reading &reading)
{
    Status status;
    status.message            = message;
    status.cycle              = m_cycle;
    status.cycles             = m_cycles;
    status.nozzle_temperature = reading.nozzle;
    status.nozzle_target      = nozzle_target;
    status.bed_temperature    = reading.bed;
    status.bed_target         = bed_target;
    m_on_status(status);
}

std::string HeatingCalibration::move_to(const Vec2d &point) const
{
    std::string command = "G1 X";
    command.reserve(64);
    command += float_to_string_decimal_point(point.x() - m_gcode_offset.x(), 2);
    command += " Y";
    command += float_to_string_decimal_point(point.y() - m_gcode_offset.y(), 2);
    command += " F";
    command += float_to_string_decimal_point(travel_feedrate, 0);
    return command;
}

void HeatingCalibration::cool_down(const Reading &start)
{
    run_gcode("M104 T0 S0\nM140 S0\nM106 S255");
    // In a warm room, stop cooling when the temperature stalls above the minimum.
    double nozzle_lowest = start.nozzle, nozzle_lowest_at = start.eventtime;
    double bed_lowest = start.bed, bed_lowest_at = start.eventtime;
    for (;;) {
        const Reading reading = read_heaters();
        report(_u8L("Cooling down"), m_params.nozzle_min, m_params.bed_min, reading);
        if (reading.nozzle < nozzle_lowest - cooling_stall_drop) {
            nozzle_lowest    = reading.nozzle;
            nozzle_lowest_at = reading.eventtime;
        }
        if (reading.bed < bed_lowest - cooling_stall_drop) {
            bed_lowest    = reading.bed;
            bed_lowest_at = reading.eventtime;
        }
        const bool nozzle_cool = reading.nozzle <= m_params.nozzle_min || reading.eventtime - nozzle_lowest_at > cooling_stall_time;
        const bool bed_cool    = reading.bed <= m_params.bed_min || reading.eventtime - bed_lowest_at > cooling_stall_time;
        if (nozzle_cool && bed_cool)
            return;
        sleep(1.);
    }
}

void HeatingCalibration::heat(double nozzle_target, double bed_target, HeatingRun &nozzle_run, HeatingRun &bed_run)
{
    run_gcode("M107");
    nozzle_run = HeatingRun{nozzle_target};
    bed_run    = HeatingRun{bed_target};
    const double start = read_heaters().eventtime;
    run_gcode("M140 S" + fmt_temp(bed_target) + "\nM104 T0 S" + fmt_temp(nozzle_target));

    // Klipper's settle check uses raw sensor readings unavailable through Moonraker.
    // Time the firmware waits directly while another thread samples the ramp.
    std::atomic<bool>  stop{false};
    std::atomic<bool>  sampler_failed{false};
    std::exception_ptr sampler_error;
    std::thread        sampler([&]() {
        try {
            while (!stop) {
                const Reading reading = read_heaters();
                const double  time    = reading.eventtime - start;
                nozzle_run.samples.push_back({time, reading.nozzle, reading.nozzle_power});
                bed_run.samples.push_back({time, reading.bed, reading.bed_power});
                report(_u8L("Heating"), nozzle_target, bed_target, reading);
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        } catch (...) {
            sampler_error  = std::current_exception();
            sampler_failed = true;
        }
    });
    auto wait = [&](const std::string &script) {
        m_waiting_for_firmware = true;
        try {
            check_cancelled();
            // Cancelling HTTP cannot stop a firmware wait.
            run_gcode(script, heating_timeout, false);
            m_waiting_for_firmware = false;
        } catch (...) {
            m_waiting_for_firmware = false;
            throw;
        }
        check_cancelled();
        if (sampler_failed)
            throw RuntimeError(_u8L("Connection to printer lost during heating."));
        return read_heaters().eventtime - start;
    };

    try {
        // Waiting for the faster nozzle first allows both waits to be timed.
        nozzle_run.release = wait("M109 T0 S" + fmt_temp(nozzle_target));
        bed_run.release    = wait("M190 S" + fmt_temp(bed_target));
    } catch (...) {
        stop = true;
        sampler.join();
        throw;
    }
    stop = true;
    sampler.join();

    // If the bed already settled, M190 returns immediately; use the first sample within 1 °C.
    if (bed_run.release - nozzle_run.release < 1.)
        for (const HeatingSample &sample : bed_run.samples)
            if (sample.temperature >= bed_target - 1.) {
                bed_run.release = std::min(bed_run.release, sample.time);
                break;
            }
    BOOST_LOG_TRIVIAL(info) << "Heating calibration: nozzle " << nozzle_target << " released after " << nozzle_run.release
                            << " s, bed " << bed_target << " after " << bed_run.release << " s";
}

HeatingCalibration::Result HeatingCalibration::run()
{
    if (m_params.nozzle_max - m_params.nozzle_min < 10. || m_params.bed_max - m_params.bed_min < 10.)
        throw RuntimeError(_u8L(u8"Nozzle and bed ranges must each span at least 10 \u2103."));
    const auto targets = cycle_targets(m_params);
    m_cycle  = 0;
    m_cycles = int(targets.size()) * m_params.repetitions;

    const json info = json::parse(get("/printer/info"))["result"];
    if (info.value("state", "") != "ready")
        throw RuntimeError(format(_u8L("Printer not ready: %1%"), info.value("state_message", info.value("state", ""))));

    const json status = json::parse(get("/printer/objects/query?configfile=settings&print_stats=state&toolhead=homed_axes,axis_minimum,axis_maximum&idle_timeout=idle_timeout"))
                            ["result"]["status"];
    const std::string print_state = status["print_stats"].value("state", "");
    if (print_state == "printing" || print_state == "paused")
        throw RuntimeError(_u8L("Printer is printing or paused."));

    const json &settings = status["configfile"]["settings"];
    if (!settings.contains("extruder") || !settings.contains("heater_bed"))
        throw RuntimeError(_u8L("Printer needs both [extruder] and [heater_bed] sections."));
    auto heater_info = [](const json &section) {
        HeaterInfo heater;
        heater.max_power = section.value("max_power", 1.);
        heater.min_temp  = section.value("min_temp", 0.);
        heater.max_temp  = section.value("max_temp", 0.);
        return heater;
    };
    m_nozzle = heater_info(settings["extruder"]);
    m_bed    = heater_info(settings["heater_bed"]);
    auto validate_target = [](const char *name, double target, const HeaterInfo &heater) {
        if (target != 0. && target < heater.min_temp)
            throw RuntimeError(format(_u8L(u8"%1% target is below min_temp %2% \u2103."), name, heater.min_temp));
        if (target > heater.max_temp)
            throw RuntimeError(format(_u8L(u8"%1% target exceeds max_temp %2% \u2103."), name, heater.max_temp));
    };
    for (const auto &[nozzle_target, bed_target] : targets) {
        validate_target("extruder", nozzle_target, m_nozzle);
        validate_target("heater_bed", bed_target, m_bed);
    }
    const double idle_timeout = status.contains("idle_timeout") && status["idle_timeout"].contains("idle_timeout")
                                    ? status["idle_timeout"]["idle_timeout"].get<double>()
                                    : settings.contains("idle_timeout") ? settings["idle_timeout"].value("timeout", 600.) : 600.;

    const json &toolhead = status["toolhead"];
    const Vec2d axis_min(toolhead["axis_minimum"][0].get<double>(), toolhead["axis_minimum"][1].get<double>());
    const Vec2d axis_max(toolhead["axis_maximum"][0].get<double>(), toolhead["axis_maximum"][1].get<double>());
    const double z_min = toolhead["axis_minimum"][2].get<double>();
    const double z_max = toolhead["axis_maximum"][2].get<double>();
    if (z_max < park_z || z_min > z_max)
        throw RuntimeError(_u8L("Printer Z travel must reach at least 3 mm."));
    const double park_height = std::clamp(park_z, z_min, z_max);
    const double travel_height = std::max(park_height, std::min(10., z_max));

    // Leave extra clearance for G-code coordinates rounded to 0.01 mm.
    const Vec2d safe_axis_min = axis_min + Vec2d::Constant(park_margin + 0.05);
    const Vec2d safe_axis_max = axis_max - Vec2d::Constant(park_margin + 0.05);
    if ((safe_axis_max - safe_axis_min).minCoeff() < 0.)
        throw RuntimeError(_u8L("No parking position inside printer travel limits."));
    m_park = 0.5 * (safe_axis_min + safe_axis_max);
    if (!m_params.bed_area.empty()) {
        if (m_params.bed_area.size() < 3)
            throw RuntimeError(_u8L("Printable area is not a valid polygon."));
        const Polygon area = Polygon::new_scale(m_params.bed_area);
        if (area.area() == 0.)
            throw RuntimeError(_u8L("Printable area is not a valid polygon."));
        const Polygon travel = Polygon::new_scale({safe_axis_min, {safe_axis_max.x(), safe_axis_min.y()},
                                                  safe_axis_max, {safe_axis_min.x(), safe_axis_max.y()}});
        const ExPolygons safe_area = intersection_ex(shrink_ex(Polygons{area}, float(park_margin + 0.05)), Polygons{travel});
        if (safe_area.empty())
            throw RuntimeError(_u8L("No parking position inside the printable area and printer travel limits."));
        // Park at the bed center, where bed sensors usually sit, or at the nearest safe point to it.
        const Vec2d center = BoundingBoxf(m_params.bed_area).center();
        const Point center_scaled = Point::new_scale(center);
        m_park = unscale(safe_area.front().contour.points.front());
        if (std::any_of(safe_area.begin(), safe_area.end(), [&](const ExPolygon &region) { return region.contains(center_scaled); }))
            m_park = center;
        else
            for (const ExPolygon &region : safe_area)
                for (const Point &point : region.contour.points)
                    if ((unscale(point) - center).squaredNorm() < (m_park - center).squaredNorm())
                        m_park = unscale(point);
    }

    const std::string gcode_state = "ORCASLICER_HEATING_CALIBRATION_" +
                                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    bool gcode_state_saved = false;
    auto cleanup = [&]() {
        std::exception_ptr restore_error;
        if (gcode_state_saved) {
            try {
                run_gcode("RESTORE_GCODE_STATE NAME=" + gcode_state + " MOVE=0", cleanup_timeout, false);
            } catch (...) {
                restore_error = std::current_exception();
            }
        }
        run_gcode("M104 T0 S0\nM140 S0\nM107\nSET_IDLE_TIMEOUT TIMEOUT=" + json(idle_timeout).dump(), cleanup_timeout, false);
        if (restore_error)
            std::rethrow_exception(restore_error);
    };

    std::vector<HeatingRun> nozzle_runs, bed_runs;
    try {
        // Bed cooling may exceed Klipper's idle timeout, which switches off heaters and motors.
        run_gcode("SET_IDLE_TIMEOUT TIMEOUT=" + float_to_string_decimal_point(calibration_idle_timeout, 0));
        Reading reading = read_heaters();
        const std::string homed = status["toolhead"].value("homed_axes", "");
        if (homed.find('x') == std::string::npos || homed.find('y') == std::string::npos || homed.find('z') == std::string::npos) {
            report(_u8L("Homing"), 0., 0., reading);
            run_gcode("G28");
        }
        check_cancelled();
        report(_u8L("Parking nozzle"), 0., 0., reading);
        // Save after homing so restoring coordinate state does not undo its origin.
        run_gcode("SAVE_GCODE_STATE NAME=" + gcode_state, cleanup_timeout, false);
        gcode_state_saved = true;
        const json move_status = json::parse(get("/printer/objects/query?gcode_move=position,gcode_position"));
        const json &gcode_move = move_status["result"]["status"]["gcode_move"];
        for (int axis = 0; axis < 3; ++axis)
            m_gcode_offset[axis] = gcode_move["position"][axis].get<double>() - gcode_move["gcode_position"][axis].get<double>();
        run_gcode("G90\nG1 Z" + float_to_string_decimal_point(travel_height - m_gcode_offset.z(), 6) + " F600\n" +
                  move_to(m_park) + "\nG1 Z" +
                  float_to_string_decimal_point(park_height - m_gcode_offset.z(), 6) + " F600");

        for (int repetition = 0; repetition < m_params.repetitions; ++repetition)
            for (const auto &[nozzle_target, bed_target] : targets) {
                ++m_cycle;
                cool_down(read_heaters());
                nozzle_runs.emplace_back();
                bed_runs.emplace_back();
                heat(nozzle_target, bed_target, nozzle_runs.back(), bed_runs.back());
            }
    } catch (...) {
        try {
            cleanup();
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "Heating calibration: cleanup failed: " << e.what();
        }
        throw;
    }
    cleanup();

    Result result{fit_heater_curve(nozzle_runs, m_nozzle.max_power), fit_heater_curve(bed_runs, m_bed.max_power)};
    if (!HeaterCurve(result.nozzle.ramp, {}).valid() || !HeaterCurve(result.bed.ramp, {}).valid())
        throw RuntimeError(_u8L("Not enough full-power heating data to fit a curve."));
    return result;
}

} // namespace Slic3r
