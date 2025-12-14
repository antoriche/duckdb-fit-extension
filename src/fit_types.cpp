#include "include/fit_types.hpp"

namespace duckdb {

FitRecord::FitRecord()
    : timestamp(timestamp_tz_t(0)), latitude(0.0), longitude(0.0), altitude(0.0), enhanced_altitude(0.0), distance(0.0),
      speed(0.0), enhanced_speed(0.0), vertical_speed(0.0), power(0), motor_power(0), accumulated_power(0),
      compressed_accumulated_power(0), heart_rate(0), total_hemoglobin_conc(0.0), total_hemoglobin_conc_min(0.0),
      total_hemoglobin_conc_max(0.0), saturated_hemoglobin_percent(0.0), saturated_hemoglobin_percent_min(0.0),
      saturated_hemoglobin_percent_max(0.0), cadence(0), cadence256(0.0), fractional_cadence(0.0), temperature(0),
      core_temperature(0.0), grade(0.0), resistance(0), left_right_balance(0), left_torque_effectiveness(0.0),
      right_torque_effectiveness(0.0), left_pedal_smoothness(0.0), right_pedal_smoothness(0.0),
      combined_pedal_smoothness(0.0), left_pco(0), right_pco(0), vertical_oscillation(0.0), stance_time_percent(0.0),
      stance_time(0.0), stance_time_balance(0.0), step_length(0.0), vertical_ratio(0.0), cycle_length(0.0),
      cycle_length16(0.0), cycles(0), total_cycles(0), time_from_course(0.0), gps_accuracy(0), calories(0), zone(0),
      activity_type(""), stroke_type(""), time128(0.0), grit(0.0), flow(0.0), current_stress(0.0),
      ebike_travel_range(0), ebike_battery_level(0), ebike_assist_mode(0), ebike_assist_level_percent(0),
      battery_soc(0.0), ball_speed(0.0), absolute_pressure(0), depth(0.0), next_stop_depth(0.0), next_stop_time(0),
      time_to_surface(0), ndl_time(0), cns_load(0), n2_load(0), air_time_remaining(0), pressure_sac(0.0),
      volume_sac(0.0), rmv(0.0), ascent_rate(0.0), po2(0.0), respiration_rate(0), enhanced_respiration_rate(0.0),
      device_index(0), file_source("") {
}

FitActivity::FitActivity()
    : activity_id(0), file_id(""), timestamp(timestamp_tz_t(0)), local_timestamp(timestamp_tz_t(0)),
      start_time(timestamp_tz_t(0)), total_timer_time(0.0), total_elapsed_time(0.0), total_distance(0.0), sport(""),
      sub_sport(""), manufacturer(""), product(""), device_serial_number(0), software_version(""), total_calories(0),
      total_ascent(0.0), total_descent(0.0), avg_heart_rate(0), max_heart_rate(0), avg_speed(0.0), max_speed(0.0),
      avg_power(0), max_power(0), avg_cadence(0), max_cadence(0), start_position_lat(0.0), start_position_long(0.0),
      end_position_lat(0.0), end_position_long(0.0), file_source("") {
}

FitSession::FitSession()
    : session_id(0), activity_id(0), timestamp(timestamp_tz_t(0)), start_time(timestamp_tz_t(0)),
      total_elapsed_time(0.0), total_timer_time(0.0), total_distance(0.0), sport(""), sub_sport(""), total_calories(0),
      avg_speed(0.0), max_speed(0.0), avg_heart_rate(0), max_heart_rate(0), min_heart_rate(0), avg_cadence(0),
      max_cadence(0), avg_power(0), max_power(0), normalized_power(0), intensity_factor(0.0),
      training_stress_score(0.0), total_work(0), total_ascent(0.0), total_descent(0.0), first_lap_index(0), num_laps(0),
      event(""), event_type(""), trigger(""), file_source("") {
}

FitLap::FitLap()
    : lap_id(0), session_id(0), activity_id(0), timestamp(timestamp_tz_t(0)), start_time(timestamp_tz_t(0)),
      total_elapsed_time(0.0), total_timer_time(0.0), total_distance(0.0), total_calories(0), avg_speed(0.0),
      max_speed(0.0), avg_heart_rate(0), max_heart_rate(0), min_heart_rate(0), avg_cadence(0), max_cadence(0),
      avg_power(0), max_power(0), total_ascent(0.0), total_descent(0.0), lap_trigger(""), event(""), event_type(""),
      start_position_lat(0.0), start_position_long(0.0), end_position_lat(0.0), end_position_long(0.0),
      file_source("") {
}

FitDevice::FitDevice()
    : device_id(0), activity_id(0), device_index(0), device_type(""), manufacturer(""), product(""), serial_number(0),
      software_version(""), hardware_version(""), cum_operating_time(0), battery_status(""), sensor_position(""),
      descriptor(""), ant_transmission_type(0), ant_device_number(0), ant_network(""), source_type(""),
      product_name(""), battery_voltage(0.0), file_source("") {
}

FitEvent::FitEvent()
    : event_id(0), activity_id(0), timestamp(timestamp_tz_t(0)), event(""), event_type(""), data(0), data16(0),
      score(0), opponent_score(0), front_gear_num(0), front_gear(0), rear_gear_num(0), rear_gear(0), device_index(0),
      activity_type(""), start_timestamp(timestamp_tz_t(0)), file_source("") {
}

FitUser::FitUser()
    : user_id(0), gender(""), age(0), height(0.0), weight(0.0), language(""), time_zone(0), activity_class(0.0),
      running_lactate_threshold_hr(0), cycling_lactate_threshold_hr(0), swimming_lactate_threshold_hr(0),
      default_max_running_hr(0), default_max_biking_hr(0), default_max_hr(0), hr_setting(""), speed_setting(""),
      dist_setting(""), power_setting(""), position_setting(""), temperature_setting(""), local_id(0), global_id(0),
      wake_time(0), sleep_time(0), height_setting(""), weight_setting(""), resting_heart_rate(0),
      default_max_swimming_hr(0), file_source("") {
}

} // namespace duckdb
