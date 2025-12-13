#define DUCKDB_EXTENSION_MAIN

#include "fit_extension.hpp"
#include "utils.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

// FIT SDK includes
#include "fit_decode.hpp"
#include "fit_mesg_broadcaster.hpp"
#include "fit_record_mesg.hpp"
#include "fit_file_id_mesg.hpp"
#include "fit_activity_mesg.hpp"
#include "fit_session_mesg.hpp"
#include "fit_lap_mesg.hpp"
#include "fit_device_info_mesg.hpp"
#include "fit_event_mesg.hpp"
#include "fit_user_profile_mesg.hpp"

#include <fstream>
#include <vector>
#include <memory>
#include <algorithm>

#ifdef _WIN32
	#include <windows.h>
	#include <shlwapi.h>
#else
	#include <fnmatch.h>
	#include <dirent.h>
	#include <sys/stat.h>
#endif

namespace duckdb {

// Helper function to expand glob patterns
static vector<string> ExpandGlobPattern(const string &pattern) {
	vector<string> files;

	// Check if pattern contains wildcards
	if (pattern.find('*') == string::npos && pattern.find('?') == string::npos && pattern.find('[') == string::npos) {
		// No wildcards, just return the single file
		files.push_back(pattern);
		return files;
	}

#ifdef _WIN32
	// Windows implementation using FindFirstFile/FindNextFile
	size_t last_slash = pattern.find_last_of("/\\");
	string dir_path;
	string filename_pattern;

	if (last_slash != string::npos) {
		dir_path = pattern.substr(0, last_slash);
		filename_pattern = pattern.substr(last_slash + 1);
	} else {
		dir_path = ".";
		filename_pattern = pattern;
	}

	// Convert to Windows path separators
	for (auto &c : dir_path) {
		if (c == '/') c = '\\';
	}

	string search_path = dir_path + "\\" + filename_pattern;
	WIN32_FIND_DATAA find_data;
	HANDLE hFind = FindFirstFileA(search_path.c_str(), &find_data);

	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			// Skip directories
			if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				string full_path = dir_path + "\\" + find_data.cFileName;
				// Convert back to forward slashes for consistency
				for (auto &c : full_path) {
					if (c == '\\') c = '/';
				}
				files.push_back(full_path);
			}
		} while (FindNextFileA(hFind, &find_data) != 0);
		FindClose(hFind);
	}
#else
	// POSIX implementation (Linux, macOS)
	size_t last_slash = pattern.find_last_of('/');
	string dir_path;
	string filename_pattern;

	if (last_slash != string::npos) {
		dir_path = pattern.substr(0, last_slash);
		filename_pattern = pattern.substr(last_slash + 1);
	} else {
		dir_path = ".";
		filename_pattern = pattern;
	}

	// Open directory
	DIR *dir = opendir(dir_path.c_str());
	if (!dir) {
		return files; // Return empty vector if directory doesn't exist
	}

	// Read directory entries
	struct dirent *entry;
	while ((entry = readdir(dir)) != nullptr) {
		// Skip . and .. entries
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		// Check if it matches the pattern
		if (fnmatch(filename_pattern.c_str(), entry->d_name, 0) == 0) {
			// Construct full path
			string full_path;
			if (dir_path == ".") {
				full_path = entry->d_name;
			} else {
				full_path = dir_path + "/" + entry->d_name;
			}

			// Check if it's a regular file
			struct stat statbuf;
			if (stat(full_path.c_str(), &statbuf) == 0 && S_ISREG(statbuf.st_mode)) {
				files.push_back(full_path);
			}
		}
	}

	closedir(dir);
#endif

	// Sort files for consistent ordering
	std::sort(files.begin(), files.end());

	return files;
}

// Structure to hold FIT record data - aligned with documentation
struct FitRecord {
	// Basic timestamp and location
	timestamp_tz_t timestamp;
	double latitude;          // Converted from semicircles to degrees
	double longitude;         // Converted from semicircles to degrees
	double altitude;          // Meters
	double enhanced_altitude; // Meters (higher precision)

	// Speed and distance
	double distance;       // Meters (cumulative)
	double speed;          // Meters per second
	double enhanced_speed; // Meters per second (higher precision)
	double vertical_speed; // Meters per second

	// Power metrics (watts)
	uint16_t power;
	uint16_t motor_power; // E-bike motor power
	uint32_t accumulated_power;
	uint16_t compressed_accumulated_power;

	// Heart rate and physiological data
	uint8_t heart_rate; // Beats per minute (corrected from uint16_t)
	double total_hemoglobin_conc;
	double total_hemoglobin_conc_min;
	double total_hemoglobin_conc_max;
	double saturated_hemoglobin_percent;
	double saturated_hemoglobin_percent_min;
	double saturated_hemoglobin_percent_max;

	// Cadence metrics
	uint8_t cadence; // RPM/steps per minute (corrected from uint16_t)
	double cadence256;
	double fractional_cadence;

	// Temperature (Celsius)
	int8_t temperature; // Corrected to int8_t for proper range
	double core_temperature;

	// Cycling metrics
	double grade; // Percentage
	uint16_t resistance;
	uint8_t left_right_balance;        // Percentage (corrected from double)
	double left_torque_effectiveness;  // Percentage
	double right_torque_effectiveness; // Percentage
	double left_pedal_smoothness;      // Percentage
	double right_pedal_smoothness;     // Percentage
	double combined_pedal_smoothness;  // Percentage
	int8_t left_pco;                   // Platform center offset (mm)
	int8_t right_pco;                  // Platform center offset (mm)

	// Running metrics
	double vertical_oscillation; // Millimeters
	double stance_time_percent;  // Percentage
	double stance_time;          // Milliseconds
	double stance_time_balance;
	double step_length;    // Millimeters
	double vertical_ratio; // Percentage

	// Cycling/Swimming specific
	double cycle_length; // Meters
	double cycle_length16;
	uint8_t cycles;
	uint32_t total_cycles;

	// Navigation and course
	double time_from_course; // Seconds
	uint8_t gps_accuracy;    // Meters

	// Energy and calories
	uint16_t calories; // Kilocalories

	// Zones and training
	uint8_t zone;
	string activity_type;
	string stroke_type;

	// Advanced metrics
	double time128;
	double grit;
	double flow;
	double current_stress;

	// E-bike specific
	uint16_t ebike_travel_range;
	uint8_t ebike_battery_level;
	uint8_t ebike_assist_mode;
	uint8_t ebike_assist_level_percent;
	double battery_soc; // Percentage

	// Sports specific
	double ball_speed; // Meters per second

	// Diving/Swimming specific
	uint32_t absolute_pressure;  // Pascals
	double depth;                // Meters
	double next_stop_depth;      // Meters
	uint32_t next_stop_time;     // Seconds
	uint32_t time_to_surface;    // Seconds
	uint32_t ndl_time;           // Seconds (No Decompression Limit)
	uint8_t cns_load;            // Percentage
	uint16_t n2_load;            // Percentage
	uint32_t air_time_remaining; // Seconds
	double pressure_sac;         // Pressure Surface Air Consumption
	double volume_sac;           // Volume Surface Air Consumption
	double rmv;                  // Respiratory Minute Volume
	double ascent_rate;          // Meters per second
	double po2;                  // Partial pressure O2

	// Respiratory
	uint8_t respiration_rate; // Breaths per minute
	double enhanced_respiration_rate;

	// Device info
	uint8_t device_index;

	// File source information
	string file_source;

	// Initialize all fields to invalid/zero values
	FitRecord()
	    : timestamp(timestamp_tz_t()), latitude(0.0), longitude(0.0), altitude(0.0), enhanced_altitude(0.0),
	      distance(0.0), speed(0.0), enhanced_speed(0.0), vertical_speed(0.0), power(0), motor_power(0),
	      accumulated_power(0), compressed_accumulated_power(0), heart_rate(0), total_hemoglobin_conc(0.0),
	      total_hemoglobin_conc_min(0.0), total_hemoglobin_conc_max(0.0), saturated_hemoglobin_percent(0.0),
	      saturated_hemoglobin_percent_min(0.0), saturated_hemoglobin_percent_max(0.0), cadence(0), cadence256(0.0),
	      fractional_cadence(0.0), temperature(0), core_temperature(0.0), grade(0.0), resistance(0),
	      left_right_balance(0), left_torque_effectiveness(0.0), right_torque_effectiveness(0.0),
	      left_pedal_smoothness(0.0), right_pedal_smoothness(0.0), combined_pedal_smoothness(0.0), left_pco(0),
	      right_pco(0), vertical_oscillation(0.0), stance_time_percent(0.0), stance_time(0.0), stance_time_balance(0.0),
	      step_length(0.0), vertical_ratio(0.0), cycle_length(0.0), cycle_length16(0.0), cycles(0), total_cycles(0),
	      time_from_course(0.0), gps_accuracy(0), calories(0), zone(0), activity_type(""), stroke_type(""),
	      time128(0.0), grit(0.0), flow(0.0), current_stress(0.0), ebike_travel_range(0), ebike_battery_level(0),
	      ebike_assist_mode(0), ebike_assist_level_percent(0), battery_soc(0.0), ball_speed(0.0), absolute_pressure(0),
	      depth(0.0), next_stop_depth(0.0), next_stop_time(0), time_to_surface(0), ndl_time(0), cns_load(0), n2_load(0),
	      air_time_remaining(0), pressure_sac(0.0), volume_sac(0.0), rmv(0.0), ascent_rate(0.0), po2(0.0),
	      respiration_rate(0), enhanced_respiration_rate(0.0), device_index(0), file_source("") {
	}
};

// Structure to hold FIT activity metadata
struct FitActivity {
	uint64_t activity_id;
	string file_id;
	timestamp_tz_t timestamp;
	timestamp_tz_t local_timestamp;
	timestamp_tz_t start_time;
	double total_timer_time;
	double total_elapsed_time;
	double total_distance;
	string sport;
	string sub_sport;
	string manufacturer;
	string product;
	uint64_t device_serial_number;
	string software_version;
	uint32_t total_calories;
	double total_ascent;
	double total_descent;
	uint8_t avg_heart_rate;
	uint8_t max_heart_rate;
	double avg_speed;
	double max_speed;
	uint16_t avg_power;
	uint16_t max_power;
	uint8_t avg_cadence;
	uint8_t max_cadence;
	double start_position_lat;
	double start_position_long;
	double end_position_lat;
	double end_position_long;
	string file_source;

	FitActivity()
	    : activity_id(0), file_id(""), timestamp(timestamp_tz_t()), local_timestamp(timestamp_tz_t()),
	      start_time(timestamp_tz_t()), total_timer_time(0.0), total_elapsed_time(0.0), total_distance(0.0), sport(""),
	      sub_sport(""), manufacturer(""), product(""), device_serial_number(0), software_version(""),
	      total_calories(0), total_ascent(0.0), total_descent(0.0), avg_heart_rate(0), max_heart_rate(0),
	      avg_speed(0.0), max_speed(0.0), avg_power(0), max_power(0), avg_cadence(0), max_cadence(0),
	      start_position_lat(0.0), start_position_long(0.0), end_position_lat(0.0), end_position_long(0.0),
	      file_source("") {
	}
};

// Structure to hold FIT session data
struct FitSession {
	uint32_t session_id;
	uint64_t activity_id;
	timestamp_tz_t timestamp;
	timestamp_tz_t start_time;
	double total_elapsed_time;
	double total_timer_time;
	double total_distance;
	string sport;
	string sub_sport;
	uint32_t total_calories;
	double avg_speed;
	double max_speed;
	uint8_t avg_heart_rate;
	uint8_t max_heart_rate;
	uint8_t min_heart_rate;
	uint8_t avg_cadence;
	uint8_t max_cadence;
	uint16_t avg_power;
	uint16_t max_power;
	uint16_t normalized_power;
	double intensity_factor;
	double training_stress_score;
	uint32_t total_work;
	double total_ascent;
	double total_descent;
	uint8_t first_lap_index;
	uint8_t num_laps;
	string event;
	string event_type;
	string trigger;
	string file_source;

	FitSession()
	    : session_id(0), activity_id(0), timestamp(timestamp_tz_t()), start_time(timestamp_tz_t()),
	      total_elapsed_time(0.0), total_timer_time(0.0), total_distance(0.0), sport(""), sub_sport(""),
	      total_calories(0), avg_speed(0.0), max_speed(0.0), avg_heart_rate(0), max_heart_rate(0), min_heart_rate(0),
	      avg_cadence(0), max_cadence(0), avg_power(0), max_power(0), normalized_power(0), intensity_factor(0.0),
	      training_stress_score(0.0), total_work(0), total_ascent(0.0), total_descent(0.0), first_lap_index(0),
	      num_laps(0), event(""), event_type(""), trigger(""), file_source("") {
	}
};

// Structure to hold FIT lap data
struct FitLap {
	uint32_t lap_id;
	uint32_t session_id;
	uint64_t activity_id;
	timestamp_tz_t timestamp;
	timestamp_tz_t start_time;
	double total_elapsed_time;
	double total_timer_time;
	double total_distance;
	uint32_t total_calories;
	double avg_speed;
	double max_speed;
	uint8_t avg_heart_rate;
	uint8_t max_heart_rate;
	uint8_t min_heart_rate;
	uint8_t avg_cadence;
	uint8_t max_cadence;
	uint16_t avg_power;
	uint16_t max_power;
	double total_ascent;
	double total_descent;
	string lap_trigger;
	string event;
	string event_type;
	double start_position_lat;
	double start_position_long;
	double end_position_lat;
	double end_position_long;
	string file_source;

	FitLap()
	    : lap_id(0), session_id(0), activity_id(0), timestamp(timestamp_tz_t()), start_time(timestamp_tz_t()),
	      total_elapsed_time(0.0), total_timer_time(0.0), total_distance(0.0), total_calories(0), avg_speed(0.0),
	      max_speed(0.0), avg_heart_rate(0), max_heart_rate(0), min_heart_rate(0), avg_cadence(0), max_cadence(0),
	      avg_power(0), max_power(0), total_ascent(0.0), total_descent(0.0), lap_trigger(""), event(""), event_type(""),
	      start_position_lat(0.0), start_position_long(0.0), end_position_lat(0.0), end_position_long(0.0),
	      file_source("") {
	}
};

// Structure to hold FIT device info
struct FitDevice {
	uint32_t device_id;
	uint64_t activity_id;
	uint8_t device_index;
	string device_type;
	string manufacturer;
	string product;
	uint64_t serial_number;
	string software_version;
	string hardware_version;
	uint32_t cum_operating_time;
	string battery_status;
	string sensor_position;
	string descriptor;
	uint8_t ant_transmission_type;
	uint16_t ant_device_number;
	string ant_network;
	string source_type;
	string product_name;
	double battery_voltage;
	string file_source;

	FitDevice()
	    : device_id(0), activity_id(0), device_index(0), device_type(""), manufacturer(""), product(""),
	      serial_number(0), software_version(""), hardware_version(""), cum_operating_time(0), battery_status(""),
	      sensor_position(""), descriptor(""), ant_transmission_type(0), ant_device_number(0), ant_network(""),
	      source_type(""), product_name(""), battery_voltage(0.0), file_source("") {
	}
};

// Structure to hold FIT event data
struct FitEvent {
	uint32_t event_id;
	uint64_t activity_id;
	timestamp_tz_t timestamp;
	string event;
	string event_type;
	uint32_t data;
	uint16_t data16;
	uint16_t score;
	uint16_t opponent_score;
	uint8_t front_gear_num;
	uint8_t front_gear;
	uint8_t rear_gear_num;
	uint8_t rear_gear;
	uint8_t device_index;
	string activity_type;
	timestamp_tz_t start_timestamp;
	string file_source;

	FitEvent()
	    : event_id(0), activity_id(0), timestamp(timestamp_tz_t()), event(""), event_type(""), data(0), data16(0),
	      score(0), opponent_score(0), front_gear_num(0), front_gear(0), rear_gear_num(0), rear_gear(0),
	      device_index(0), activity_type(""), start_timestamp(timestamp_tz_t()), file_source("") {
	}
};

// Structure to hold FIT user profile data
struct FitUser {
	uint32_t user_id;
	string gender;
	uint8_t age;
	double height;
	double weight;
	string language;
	int8_t time_zone;
	double activity_class;
	uint8_t running_lactate_threshold_hr;
	uint8_t cycling_lactate_threshold_hr;
	uint8_t swimming_lactate_threshold_hr;
	uint8_t default_max_running_hr;
	uint8_t default_max_biking_hr;
	uint8_t default_max_hr;
	string hr_setting;
	string speed_setting;
	string dist_setting;
	string power_setting;
	string position_setting;
	string temperature_setting;
	uint32_t local_id;
	uint64_t global_id;
	uint32_t wake_time;
	uint32_t sleep_time;
	string height_setting;
	string weight_setting;
	uint8_t resting_heart_rate;
	uint8_t default_max_swimming_hr;
	string file_source;

	FitUser()
	    : user_id(0), gender(""), age(0), height(0.0), weight(0.0), language(""), time_zone(0), activity_class(0.0),
	      running_lactate_threshold_hr(0), cycling_lactate_threshold_hr(0), swimming_lactate_threshold_hr(0),
	      default_max_running_hr(0), default_max_biking_hr(0), default_max_hr(0), hr_setting(""), speed_setting(""),
	      dist_setting(""), power_setting(""), position_setting(""), temperature_setting(""), local_id(0), global_id(0),
	      wake_time(0), sleep_time(0), height_setting(""), weight_setting(""), resting_heart_rate(0),
	      default_max_swimming_hr(0), file_source("") {
	}
};

// FIT message listener to collect all types of data
class FitDataCollector : public fit::RecordMesgListener,
                         public fit::FileIdMesgListener,
                         public fit::ActivityMesgListener,
                         public fit::SessionMesgListener,
                         public fit::LapMesgListener,
                         public fit::DeviceInfoMesgListener,
                         public fit::EventMesgListener,
                         public fit::UserProfileMesgListener {
public:
	std::vector<FitRecord> records;
	std::vector<FitActivity> activities;
	std::vector<FitSession> sessions;
	std::vector<FitLap> laps;
	std::vector<FitDevice> devices;
	std::vector<FitEvent> events;
	std::vector<FitUser> users;
	string file_type;
	string manufacturer;
	string activity_name;
	string current_activity_type; // Track current activity type for records
	string current_file_source;   // Track current file being processed

	FitDataCollector() : current_activity_type(""), current_file_source("") {
	}

	// Method to set the current file being processed
	void SetCurrentFile(const string &file_path) {
		current_file_source = file_path;
	}

	void OnMesg(fit::RecordMesg &record) override {
		FitRecord fitRecord = {};

		// Timestamp
		if (record.IsTimestampValid()) {
			// Convert FIT timestamp (seconds since 1989-12-31 UTC) to DuckDB timestamp with timezone
			// FIT epoch: 1989-12-31 00:00:00 UTC = 631065600 seconds since Unix epoch (1970-01-01)
			uint32_t fit_timestamp = record.GetTimestamp();
			int64_t unix_timestamp = fit_timestamp + 631065600; // Convert to Unix timestamp
			timestamp_t ts = Timestamp::FromEpochSeconds(unix_timestamp);
			timestamp_tz_t ts_with_tz = timestamp_tz_t(ts);
			fitRecord.timestamp = ts_with_tz;
		}

		// Position - Convert semicircles to degrees properly
		if (record.IsPositionLatValid()) {
			int32_t lat_semicircles = record.GetPositionLat();
			if (lat_semicircles != 0x7FFFFFFF) { // Check for invalid value
				fitRecord.latitude = lat_semicircles * (180.0 / 2147483648.0);
			}
		}
		if (record.IsPositionLongValid()) {
			int32_t long_semicircles = record.GetPositionLong();
			if (long_semicircles != 0x7FFFFFFF) { // Check for invalid value
				fitRecord.longitude = long_semicircles * (180.0 / 2147483648.0);
			}
		}

		// Altitude
		if (record.IsAltitudeValid()) {
			uint16_t alt = record.GetAltitude();
			if (alt != 0xFFFF) {                          // Check for invalid value
				fitRecord.altitude = (alt / 5.0) - 500.0; // Convert from scaled value
			}
		}
		if (record.IsEnhancedAltitudeValid()) {
			uint32_t enh_alt = record.GetEnhancedAltitude();
			if (enh_alt != 0xFFFFFFFF) {                               // Check for invalid value
				fitRecord.enhanced_altitude = (enh_alt / 5.0) - 500.0; // Convert from scaled value
			}
		}

		// Speed and distance
		if (record.IsDistanceValid()) {
			uint32_t dist = record.GetDistance();
			if (dist != 0xFFFFFFFF) {              // Check for invalid value
				fitRecord.distance = dist / 100.0; // Convert from scaled value (cm to m)
			}
		}
		if (record.IsSpeedValid()) {
			uint16_t spd = record.GetSpeed();
			if (spd != 0xFFFF) {                // Check for invalid value
				fitRecord.speed = spd / 1000.0; // Convert from scaled value (mm/s to m/s)
			}
		}
		if (record.IsEnhancedSpeedValid()) {
			uint32_t enh_spd = record.GetEnhancedSpeed();
			if (enh_spd != 0xFFFFFFFF) {                     // Check for invalid value
				fitRecord.enhanced_speed = enh_spd / 1000.0; // Convert from scaled value
			}
		}
		if (record.IsVerticalSpeedValid()) {
			int16_t vert_spd = record.GetVerticalSpeed();
			if (vert_spd != 0x7FFF) {                         // Check for invalid value
				fitRecord.vertical_speed = vert_spd / 1000.0; // Convert from scaled value
			}
		}

		// Power metrics
		if (record.IsPowerValid()) {
			uint16_t pwr = record.GetPower();
			if (pwr != 0xFFFF) { // Check for invalid value
				fitRecord.power = pwr;
			}
		}
		if (record.IsMotorPowerValid()) {
			uint16_t motor_pwr = record.GetMotorPower();
			if (motor_pwr != 0xFFFF) { // Check for invalid value
				fitRecord.motor_power = motor_pwr;
			}
		}
		if (record.IsAccumulatedPowerValid()) {
			uint32_t acc_pwr = record.GetAccumulatedPower();
			if (acc_pwr != 0xFFFFFFFF) { // Check for invalid value
				fitRecord.accumulated_power = acc_pwr;
			}
		}
		if (record.IsCompressedAccumulatedPowerValid()) {
			uint16_t comp_acc_pwr = record.GetCompressedAccumulatedPower();
			if (comp_acc_pwr != 0xFFFF) { // Check for invalid value
				fitRecord.compressed_accumulated_power = comp_acc_pwr;
			}
		}

		// Heart rate and physiological
		if (record.IsHeartRateValid()) {
			uint8_t hr = record.GetHeartRate();
			if (hr != 0xFF) { // Check for invalid value
				fitRecord.heart_rate = hr;
			}
		}
		if (record.IsTotalHemoglobinConcValid()) {
			uint16_t thc = record.GetTotalHemoglobinConc();
			if (thc != 0xFFFF) {                               // Check for invalid value
				fitRecord.total_hemoglobin_conc = thc / 100.0; // Convert from scaled value
			}
		}
		if (record.IsTotalHemoglobinConcMinValid()) {
			uint16_t thc_min = record.GetTotalHemoglobinConcMin();
			if (thc_min != 0xFFFF) { // Check for invalid value
				fitRecord.total_hemoglobin_conc_min = thc_min / 100.0;
			}
		}
		if (record.IsTotalHemoglobinConcMaxValid()) {
			uint16_t thc_max = record.GetTotalHemoglobinConcMax();
			if (thc_max != 0xFFFF) { // Check for invalid value
				fitRecord.total_hemoglobin_conc_max = thc_max / 100.0;
			}
		}
		if (record.IsSaturatedHemoglobinPercentValid()) {
			uint16_t shp = record.GetSaturatedHemoglobinPercent();
			if (shp != 0xFFFF) {                                     // Check for invalid value
				fitRecord.saturated_hemoglobin_percent = shp / 10.0; // Convert from scaled value
			}
		}
		if (record.IsSaturatedHemoglobinPercentMinValid()) {
			uint16_t shp_min = record.GetSaturatedHemoglobinPercentMin();
			if (shp_min != 0xFFFF) { // Check for invalid value
				fitRecord.saturated_hemoglobin_percent_min = shp_min / 10.0;
			}
		}
		if (record.IsSaturatedHemoglobinPercentMaxValid()) {
			uint16_t shp_max = record.GetSaturatedHemoglobinPercentMax();
			if (shp_max != 0xFFFF) { // Check for invalid value
				fitRecord.saturated_hemoglobin_percent_max = shp_max / 10.0;
			}
		}

		// Cadence
		if (record.IsCadenceValid()) {
			uint8_t cad = record.GetCadence();
			if (cad != 0xFF) { // Check for invalid value
				fitRecord.cadence = cad;
			}
		}
		if (record.IsCadence256Valid()) {
			uint16_t cad256 = record.GetCadence256();
			if (cad256 != 0xFFFF) {                    // Check for invalid value
				fitRecord.cadence256 = cad256 / 256.0; // Convert from scaled value
			}
		}
		if (record.IsFractionalCadenceValid()) {
			uint8_t frac_cad = record.GetFractionalCadence();
			if (frac_cad != 0xFF) {                              // Check for invalid value
				fitRecord.fractional_cadence = frac_cad / 128.0; // Convert from scaled value
			}
		}

		// Temperature
		if (record.IsTemperatureValid()) {
			int8_t temp = record.GetTemperature();
			if (temp != 0x7F) {               // Check for invalid value
				fitRecord.temperature = temp; // Already in Celsius
			}
		}
		if (record.IsCoreTemperatureValid()) {
			uint16_t core_temp = record.GetCoreTemperature();
			if (core_temp != 0xFFFF) {                          // Check for invalid value
				fitRecord.core_temperature = core_temp / 100.0; // Convert from scaled value
			}
		}

		// Cycling metrics
		if (record.IsGradeValid()) {
			int16_t grd = record.GetGrade();
			if (grd != 0x7FFF) {               // Check for invalid value
				fitRecord.grade = grd / 100.0; // Convert from scaled value (percentage)
			}
		}
		if (record.IsResistanceValid()) {
			uint8_t res = record.GetResistance();
			if (res != 0xFF) { // Check for invalid value
				fitRecord.resistance = res;
			}
		}
		if (record.IsLeftRightBalanceValid()) {
			uint8_t lr_bal = record.GetLeftRightBalance();
			if (lr_bal != 0xFF) {                      // Check for invalid value
				fitRecord.left_right_balance = lr_bal; // Already in percentage
			}
		}
		if (record.IsLeftTorqueEffectivenessValid()) {
			uint8_t lte = record.GetLeftTorqueEffectiveness();
			if (lte != 0xFF) {                                   // Check for invalid value
				fitRecord.left_torque_effectiveness = lte / 2.0; // Convert from scaled value
			}
		}
		if (record.IsRightTorqueEffectivenessValid()) {
			uint8_t rte = record.GetRightTorqueEffectiveness();
			if (rte != 0xFF) {                                    // Check for invalid value
				fitRecord.right_torque_effectiveness = rte / 2.0; // Convert from scaled value
			}
		}
		if (record.IsLeftPedalSmoothnessValid()) {
			uint8_t lps = record.GetLeftPedalSmoothness();
			if (lps != 0xFF) {                               // Check for invalid value
				fitRecord.left_pedal_smoothness = lps / 2.0; // Convert from scaled value
			}
		}
		if (record.IsRightPedalSmoothnessValid()) {
			uint8_t rps = record.GetRightPedalSmoothness();
			if (rps != 0xFF) {                                // Check for invalid value
				fitRecord.right_pedal_smoothness = rps / 2.0; // Convert from scaled value
			}
		}
		if (record.IsCombinedPedalSmoothnessValid()) {
			uint8_t cps = record.GetCombinedPedalSmoothness();
			if (cps != 0xFF) {                                   // Check for invalid value
				fitRecord.combined_pedal_smoothness = cps / 2.0; // Convert from scaled value
			}
		}
		if (record.IsLeftPcoValid()) {
			int8_t lpco = record.GetLeftPco();
			if (lpco != 0x7F) {            // Check for invalid value
				fitRecord.left_pco = lpco; // Already in mm
			}
		}
		if (record.IsRightPcoValid()) {
			int8_t rpco = record.GetRightPco();
			if (rpco != 0x7F) {             // Check for invalid value
				fitRecord.right_pco = rpco; // Already in mm
			}
		}

		// Running metrics
		if (record.IsVerticalOscillationValid()) {
			uint16_t vo = record.GetVerticalOscillation();
			if (vo != 0xFFFF) {                             // Check for invalid value
				fitRecord.vertical_oscillation = vo / 10.0; // Convert from scaled value (mm)
			}
		}
		if (record.IsStanceTimePercentValid()) {
			uint16_t stp = record.GetStanceTimePercent();
			if (stp != 0xFFFF) {                             // Check for invalid value
				fitRecord.stance_time_percent = stp / 100.0; // Convert from scaled value
			}
		}
		if (record.IsStanceTimeValid()) {
			uint16_t st = record.GetStanceTime();
			if (st != 0xFFFF) {                    // Check for invalid value
				fitRecord.stance_time = st / 10.0; // Convert from scaled value (ms)
			}
		}
		if (record.IsStanceTimeBalanceValid()) {
			uint16_t stb = record.GetStanceTimeBalance();
			if (stb != 0xFFFF) {                             // Check for invalid value
				fitRecord.stance_time_balance = stb / 100.0; // Convert from scaled value
			}
		}
		if (record.IsStepLengthValid()) {
			uint16_t sl = record.GetStepLength();
			if (sl != 0xFFFF) {                    // Check for invalid value
				fitRecord.step_length = sl / 10.0; // Convert from scaled value (mm)
			}
		}
		if (record.IsVerticalRatioValid()) {
			uint16_t vr = record.GetVerticalRatio();
			if (vr != 0xFFFF) {                        // Check for invalid value
				fitRecord.vertical_ratio = vr / 100.0; // Convert from scaled value
			}
		}

		// Continue with other fields with proper invalid value checking...
		// (Rest of the fields follow the same pattern)

		// Set activity type from current session
		fitRecord.activity_type = current_activity_type;
		fitRecord.file_source = current_file_source;

		records.push_back(fitRecord);
	}

	void OnMesg(fit::FileIdMesg &file_id) override {
		// Create activity from file ID information
		FitActivity activity = {};
		activity.activity_id = static_cast<uint64_t>(file_id.GetSerialNumber());

		if (file_id.IsTimeCreatedValid()) {
			uint32_t fit_timestamp = file_id.GetTimeCreated();
			int64_t unix_timestamp = fit_timestamp + 631065600;
			timestamp_t ts = Timestamp::FromEpochSeconds(unix_timestamp);
			activity.timestamp = timestamp_tz_t(ts);
		}

		// Set file ID number if available
		if (file_id.IsNumberValid()) {
			activity.file_id = std::to_string(file_id.GetNumber());
		}

		// Set manufacturer and product info
		if (file_id.IsManufacturerValid()) {
			activity.manufacturer = ConvertManufacturerToString(file_id.GetManufacturer());
		}

		// Try to get product name first, then fall back to product ID
		if (file_id.IsProductNameValid()) {
			std::wstring product_name = file_id.GetProductName();
			// Convert wstring to string (simplified conversion)
			activity.product = std::string(product_name.begin(), product_name.end());
		} else if (file_id.IsProductValid()) {
			activity.product = std::to_string(file_id.GetProduct());
		}

		if (file_id.IsSerialNumberValid()) {
			activity.device_serial_number = file_id.GetSerialNumber();
		}

		activity.file_source = current_file_source;
		activities.push_back(activity);
	}

	void OnMesg(fit::ActivityMesg &activity) override {
		// Update activity information if we have one
		if (!activities.empty()) {
			auto &act = activities.back();
			activity_name = "Activity";

			if (activity.IsTimestampValid()) {
				uint32_t fit_timestamp = activity.GetTimestamp();
				int64_t unix_timestamp = fit_timestamp + 631065600;
				timestamp_t ts = Timestamp::FromEpochSeconds(unix_timestamp);
				act.timestamp = timestamp_tz_t(ts);
			}

			if (activity.IsTotalTimerTimeValid()) {
				act.total_timer_time = activity.GetTotalTimerTime() / 1000.0; // Convert to seconds
			}

			if (activity.IsLocalTimestampValid()) {
				uint32_t fit_timestamp = activity.GetLocalTimestamp();
				int64_t unix_timestamp = fit_timestamp + 631065600;
				timestamp_t ts = Timestamp::FromEpochSeconds(unix_timestamp);
				act.local_timestamp = timestamp_tz_t(ts);
			}
		}
	}

	void OnMesg(fit::SessionMesg &session) override {
		FitSession fit_session = {};

		// Set activity ID if we have activities
		if (!activities.empty()) {
			fit_session.activity_id = activities.back().activity_id;
		}

		fit_session.session_id = sessions.size(); // Simple incremental ID

		if (session.IsTimestampValid()) {
			uint32_t fit_timestamp = session.GetTimestamp();
			int64_t unix_timestamp = fit_timestamp + 631065600;
			timestamp_t ts = Timestamp::FromEpochSeconds(unix_timestamp);
			fit_session.timestamp = timestamp_tz_t(ts);
		}

		if (session.IsStartTimeValid()) {
			uint32_t fit_timestamp = session.GetStartTime();
			int64_t unix_timestamp = fit_timestamp + 631065600;
			timestamp_t ts = Timestamp::FromEpochSeconds(unix_timestamp);
			fit_session.start_time = timestamp_tz_t(ts);
		}

		if (session.IsTotalElapsedTimeValid()) {
			fit_session.total_elapsed_time = session.GetTotalElapsedTime() / 1000.0;
		}

		if (session.IsTotalTimerTimeValid()) {
			fit_session.total_timer_time = session.GetTotalTimerTime() / 1000.0;
		}

		if (session.IsTotalDistanceValid()) {
			fit_session.total_distance = session.GetTotalDistance() / 100.0;
		}

		if (session.IsSportValid()) {
			uint8_t sport_code = session.GetSport();
			fit_session.sport = ConvertSportToString(sport_code);
			// Update current activity type for subsequent records
			current_activity_type = ConvertSportToString(sport_code);
		}

		if (session.IsSubSportValid()) {
			fit_session.sub_sport = ConvertSubSportToString(session.GetSubSport());
		}

		if (session.IsTotalCaloriesValid()) {
			fit_session.total_calories = session.GetTotalCalories();
		}

		if (session.IsAvgSpeedValid()) {
			fit_session.avg_speed = session.GetAvgSpeed() / 1000.0;
		}

		if (session.IsMaxSpeedValid()) {
			fit_session.max_speed = session.GetMaxSpeed() / 1000.0;
		}

		if (session.IsAvgHeartRateValid()) {
			fit_session.avg_heart_rate = session.GetAvgHeartRate();
		}

		if (session.IsMaxHeartRateValid()) {
			fit_session.max_heart_rate = session.GetMaxHeartRate();
		}

		if (session.IsAvgCadenceValid()) {
			fit_session.avg_cadence = session.GetAvgCadence();
		}

		if (session.IsMaxCadenceValid()) {
			fit_session.max_cadence = session.GetMaxCadence();
		}

		if (session.IsAvgPowerValid()) {
			fit_session.avg_power = session.GetAvgPower();
		}

		if (session.IsMaxPowerValid()) {
			fit_session.max_power = session.GetMaxPower();
		}

		if (session.IsNormalizedPowerValid()) {
			fit_session.normalized_power = session.GetNormalizedPower();
		}

		if (session.IsTotalAscentValid()) {
			fit_session.total_ascent = session.GetTotalAscent();
		}

		if (session.IsTotalDescentValid()) {
			fit_session.total_descent = session.GetTotalDescent();
		}

		if (session.IsNumLapsValid()) {
			fit_session.num_laps = session.GetNumLaps();
		}

		fit_session.file_source = current_file_source;
		sessions.push_back(fit_session);
	}

	void OnMesg(fit::LapMesg &lap) override {
		FitLap fit_lap = {};

		// Set activity ID and session ID
		if (!activities.empty()) {
			fit_lap.activity_id = activities.back().activity_id;
		}
		if (!sessions.empty()) {
			fit_lap.session_id = sessions.back().session_id;
		}

		fit_lap.lap_id = laps.size(); // Simple incremental ID

		if (lap.IsTimestampValid()) {
			uint32_t fit_timestamp = lap.GetTimestamp();
			int64_t unix_timestamp = fit_timestamp + 631065600;
			timestamp_t ts = Timestamp::FromEpochSeconds(unix_timestamp);
			fit_lap.timestamp = timestamp_tz_t(ts);
		}

		if (lap.IsStartTimeValid()) {
			uint32_t fit_timestamp = lap.GetStartTime();
			int64_t unix_timestamp = fit_timestamp + 631065600;
			timestamp_t ts = Timestamp::FromEpochSeconds(unix_timestamp);
			fit_lap.start_time = timestamp_tz_t(ts);
		}

		if (lap.IsTotalElapsedTimeValid()) {
			fit_lap.total_elapsed_time = lap.GetTotalElapsedTime() / 1000.0;
		}

		if (lap.IsTotalTimerTimeValid()) {
			fit_lap.total_timer_time = lap.GetTotalTimerTime() / 1000.0;
		}

		if (lap.IsTotalDistanceValid()) {
			fit_lap.total_distance = lap.GetTotalDistance() / 100.0;
		}

		if (lap.IsTotalCaloriesValid()) {
			fit_lap.total_calories = lap.GetTotalCalories();
		}

		if (lap.IsAvgSpeedValid()) {
			fit_lap.avg_speed = lap.GetAvgSpeed() / 1000.0;
		}

		if (lap.IsMaxSpeedValid()) {
			fit_lap.max_speed = lap.GetMaxSpeed() / 1000.0;
		}

		if (lap.IsAvgHeartRateValid()) {
			fit_lap.avg_heart_rate = lap.GetAvgHeartRate();
		}

		if (lap.IsMaxHeartRateValid()) {
			fit_lap.max_heart_rate = lap.GetMaxHeartRate();
		}

		if (lap.IsAvgCadenceValid()) {
			fit_lap.avg_cadence = lap.GetAvgCadence();
		}

		if (lap.IsMaxCadenceValid()) {
			fit_lap.max_cadence = lap.GetMaxCadence();
		}

		if (lap.IsAvgPowerValid()) {
			fit_lap.avg_power = lap.GetAvgPower();
		}

		if (lap.IsMaxPowerValid()) {
			fit_lap.max_power = lap.GetMaxPower();
		}

		if (lap.IsTotalAscentValid()) {
			fit_lap.total_ascent = lap.GetTotalAscent();
		}

		if (lap.IsTotalDescentValid()) {
			fit_lap.total_descent = lap.GetTotalDescent();
		}

		if (lap.IsStartPositionLatValid()) {
			int32_t lat_semicircles = lap.GetStartPositionLat();
			if (lat_semicircles != 0x7FFFFFFF) {
				fit_lap.start_position_lat = lat_semicircles * (180.0 / 2147483648.0);
			}
		}

		if (lap.IsStartPositionLongValid()) {
			int32_t long_semicircles = lap.GetStartPositionLong();
			if (long_semicircles != 0x7FFFFFFF) {
				fit_lap.start_position_long = long_semicircles * (180.0 / 2147483648.0);
			}
		}

		if (lap.IsEndPositionLatValid()) {
			int32_t lat_semicircles = lap.GetEndPositionLat();
			if (lat_semicircles != 0x7FFFFFFF) {
				fit_lap.end_position_lat = lat_semicircles * (180.0 / 2147483648.0);
			}
		}

		if (lap.IsEndPositionLongValid()) {
			int32_t long_semicircles = lap.GetEndPositionLong();
			if (long_semicircles != 0x7FFFFFFF) {
				fit_lap.end_position_long = long_semicircles * (180.0 / 2147483648.0);
			}
		}

		fit_lap.file_source = current_file_source;
		laps.push_back(fit_lap);
	}

	void OnMesg(fit::DeviceInfoMesg &device_info) override {
		FitDevice fit_device = {};

		// Set activity ID
		if (!activities.empty()) {
			fit_device.activity_id = activities.back().activity_id;
		}

		fit_device.device_id = devices.size(); // Simple incremental ID

		if (device_info.IsDeviceIndexValid()) {
			fit_device.device_index = device_info.GetDeviceIndex();
		}

		if (device_info.IsDeviceTypeValid()) {
			fit_device.device_type = std::to_string(device_info.GetDeviceType());
		}

		if (device_info.IsManufacturerValid()) {
			fit_device.manufacturer = ConvertManufacturerToString(device_info.GetManufacturer());
		}

		if (device_info.IsProductValid()) {
			fit_device.product = std::to_string(device_info.GetProduct());
		}

		if (device_info.IsSerialNumberValid()) {
			fit_device.serial_number = device_info.GetSerialNumber();
		}

		if (device_info.IsSoftwareVersionValid()) {
			fit_device.software_version = std::to_string(device_info.GetSoftwareVersion() / 100.0);
		}

		if (device_info.IsHardwareVersionValid()) {
			fit_device.hardware_version = std::to_string(device_info.GetHardwareVersion());
		}

		if (device_info.IsCumOperatingTimeValid()) {
			fit_device.cum_operating_time = device_info.GetCumOperatingTime();
		}

		if (device_info.IsBatteryStatusValid()) {
			fit_device.battery_status = std::to_string(device_info.GetBatteryStatus());
		}

		if (device_info.IsSensorPositionValid()) {
			fit_device.sensor_position = std::to_string(device_info.GetSensorPosition());
		}

		if (device_info.IsDescriptorValid()) {
			// Convert wide string to regular string - simplified approach
			fit_device.descriptor = ""; // Skip for now due to wide string conversion
		}

		if (device_info.IsAntTransmissionTypeValid()) {
			fit_device.ant_transmission_type = device_info.GetAntTransmissionType();
		}

		if (device_info.IsAntDeviceNumberValid()) {
			fit_device.ant_device_number = device_info.GetAntDeviceNumber();
		}

		if (device_info.IsAntNetworkValid()) {
			fit_device.ant_network = std::to_string(device_info.GetAntNetwork());
		}

		if (device_info.IsSourceTypeValid()) {
			fit_device.source_type = std::to_string(device_info.GetSourceType());
		}

		if (device_info.IsProductNameValid()) {
			// Convert wide string to regular string - simplified approach
			fit_device.product_name = ""; // Skip for now due to wide string conversion
		}

		if (device_info.IsBatteryVoltageValid()) {
			fit_device.battery_voltage = device_info.GetBatteryVoltage() / 256.0;
		}

		fit_device.file_source = current_file_source;
		devices.push_back(fit_device);
	}

	void OnMesg(fit::EventMesg &event) override {
		FitEvent fit_event = {};

		// Set activity ID
		if (!activities.empty()) {
			fit_event.activity_id = activities.back().activity_id;
		}

		fit_event.event_id = events.size(); // Simple incremental ID

		if (event.IsTimestampValid()) {
			uint32_t fit_timestamp = event.GetTimestamp();
			int64_t unix_timestamp = fit_timestamp + 631065600;
			timestamp_t ts = Timestamp::FromEpochSeconds(unix_timestamp);
			fit_event.timestamp = timestamp_tz_t(ts);
		}

		if (event.IsEventValid()) {
			fit_event.event = std::to_string(event.GetEvent());
		}

		if (event.IsEventTypeValid()) {
			fit_event.event_type = std::to_string(event.GetEventType());
		}

		if (event.IsDataValid()) {
			fit_event.data = event.GetData();
		}

		if (event.IsData16Valid()) {
			fit_event.data16 = event.GetData16();
		}

		if (event.IsScoreValid()) {
			fit_event.score = event.GetScore();
		}

		if (event.IsOpponentScoreValid()) {
			fit_event.opponent_score = event.GetOpponentScore();
		}

		if (event.IsFrontGearNumValid()) {
			fit_event.front_gear_num = event.GetFrontGearNum();
		}

		if (event.IsFrontGearValid()) {
			fit_event.front_gear = event.GetFrontGear();
		}

		if (event.IsRearGearNumValid()) {
			fit_event.rear_gear_num = event.GetRearGearNum();
		}

		if (event.IsRearGearValid()) {
			fit_event.rear_gear = event.GetRearGear();
		}

		if (event.IsDeviceIndexValid()) {
			fit_event.device_index = event.GetDeviceIndex();
		}

		fit_event.file_source = current_file_source;
		events.push_back(fit_event);
	}

	void OnMesg(fit::UserProfileMesg &user_profile) override {
		FitUser fit_user = {};

		fit_user.user_id = users.size(); // Simple incremental ID

		if (user_profile.IsGenderValid()) {
			fit_user.gender = std::to_string(user_profile.GetGender());
		}

		if (user_profile.IsAgeValid()) {
			fit_user.age = user_profile.GetAge();
		}

		if (user_profile.IsHeightValid()) {
			fit_user.height = user_profile.GetHeight() / 100.0; // Convert to meters
		}

		if (user_profile.IsWeightValid()) {
			fit_user.weight = user_profile.GetWeight() / 10.0; // Convert to kg
		}

		if (user_profile.IsLanguageValid()) {
			fit_user.language = std::to_string(user_profile.GetLanguage());
		}

		// Note: TimeZone field may not be available in this SDK version
		// Skip timezone for now

		if (user_profile.IsActivityClassValid()) {
			fit_user.activity_class = user_profile.GetActivityClass() / 10.0;
		}

		// Note: Lactate threshold fields may not be available in this SDK version
		// Skip lactate thresholds for now

		if (user_profile.IsDefaultMaxRunningHeartRateValid()) {
			fit_user.default_max_running_hr = user_profile.GetDefaultMaxRunningHeartRate();
		}

		if (user_profile.IsDefaultMaxBikingHeartRateValid()) {
			fit_user.default_max_biking_hr = user_profile.GetDefaultMaxBikingHeartRate();
		}

		// Note: DefaultMaxHr field may not be available in this SDK version
		// Skip for now

		if (user_profile.IsHrSettingValid()) {
			fit_user.hr_setting = std::to_string(user_profile.GetHrSetting());
		}

		if (user_profile.IsSpeedSettingValid()) {
			fit_user.speed_setting = std::to_string(user_profile.GetSpeedSetting());
		}

		if (user_profile.IsDistSettingValid()) {
			fit_user.dist_setting = std::to_string(user_profile.GetDistSetting());
		}

		if (user_profile.IsPowerSettingValid()) {
			fit_user.power_setting = std::to_string(user_profile.GetPowerSetting());
		}

		if (user_profile.IsPositionSettingValid()) {
			fit_user.position_setting = std::to_string(user_profile.GetPositionSetting());
		}

		if (user_profile.IsTemperatureSettingValid()) {
			fit_user.temperature_setting = std::to_string(user_profile.GetTemperatureSetting());
		}

		if (user_profile.IsLocalIdValid()) {
			fit_user.local_id = user_profile.GetLocalId();
		}

		// Note: GlobalId requires an index parameter - use index 0 as default
		if (user_profile.IsGlobalIdValid(0)) {
			// fit_user.global_id = user_profile.GetGlobalId(0); // Skip for now - returns FIT_BYTE
		}

		if (user_profile.IsWakeTimeValid()) {
			fit_user.wake_time = user_profile.GetWakeTime();
		}

		if (user_profile.IsSleepTimeValid()) {
			fit_user.sleep_time = user_profile.GetSleepTime();
		}

		if (user_profile.IsHeightSettingValid()) {
			fit_user.height_setting = std::to_string(user_profile.GetHeightSetting());
		}

		if (user_profile.IsWeightSettingValid()) {
			fit_user.weight_setting = std::to_string(user_profile.GetWeightSetting());
		}

		if (user_profile.IsRestingHeartRateValid()) {
			fit_user.resting_heart_rate = user_profile.GetRestingHeartRate();
		}

		// Note: DefaultMaxSwimmingHr field may not be available in this SDK version
		// Skip for now

		fit_user.file_source = current_file_source;
		users.push_back(fit_user);
	}
};

struct FitTableFunctionData : public TableFunctionData {
	string input_name;
	idx_t current_row;
	std::vector<FitRecord> fit_records;
	std::vector<FitActivity> fit_activities;
	std::vector<FitSession> fit_sessions;
	std::vector<FitLap> fit_laps;
	std::vector<FitDevice> fit_devices;
	std::vector<FitEvent> fit_events;
	std::vector<FitUser> fit_users;
	string user_timezone;
	string table_type; // To distinguish which table this data is for

	FitTableFunctionData(string name, string type = "records", ClientContext *context = nullptr)
	    : input_name(name), current_row(0), user_timezone("UTC"), table_type(type) {
		// Get user's timezone setting if context is available
		if (context) {
			Value timezone_value;
			if (context->TryGetCurrentSetting("TimeZone", timezone_value)) {
				user_timezone = timezone_value.ToString();
			}
		}
		// Load and parse FIT file
		LoadFitFile();
	}

private:
	void LoadFitFile() {
		try {
			// Check for empty or null input
			if (input_name.empty()) {
				throw std::runtime_error("File path cannot be empty");
			}

			// Expand glob pattern to get list of files
			vector<string> files = ExpandGlobPattern(input_name);

			// Check if pattern contains wildcards
			bool has_wildcards = (input_name.find('*') != string::npos || input_name.find('?') != string::npos ||
			                      input_name.find('[') != string::npos);

			if (files.empty()) {
				if (has_wildcards) {
					// For wildcard patterns, return an empty result (valid schema, 0 rows)
					fit_records.clear();
					fit_activities.clear();
					fit_sessions.clear();
					fit_laps.clear();
					fit_devices.clear();
					fit_events.clear();
					fit_users.clear();
					return;
				} else {
					// For non-wildcard patterns, throw a more specific error
					throw std::runtime_error("Cannot open FIT file: " + input_name);
				}
			}

			// For non-wildcard patterns, validate the single file exists and is readable
			if (!has_wildcards && files.size() == 1) {
				std::fstream test_file;
				test_file.open(files[0], std::ios::in | std::ios::binary);
				if (!test_file.is_open()) {
					throw std::runtime_error("Cannot open FIT file: " + input_name);
				}
				test_file.close();
			}

			// Create shared collector that will accumulate data from all files
			FitDataCollector collector;

			// Process each file
			for (const auto &file_path : files) {
				try {
					std::fstream file;
					file.open(file_path, std::ios::in | std::ios::binary);

					if (!file.is_open()) {
						if (!has_wildcards) {
							// For single files, this is an error
							throw std::runtime_error("Cannot open FIT file: " + file_path);
						}
						// For wildcard patterns, skip files that can't be opened
						continue;
					}

					// Set current file in collector
					collector.SetCurrentFile(file_path);

					fit::Decode decode;
					fit::MesgBroadcaster mesgBroadcaster;

					// Check file integrity
					if (!decode.CheckIntegrity(file)) {
						// Continue anyway, might still be readable
					}

					// Add listeners for all message types
					mesgBroadcaster.AddListener((fit::RecordMesgListener &)collector);
					mesgBroadcaster.AddListener((fit::FileIdMesgListener &)collector);
					mesgBroadcaster.AddListener((fit::ActivityMesgListener &)collector);
					mesgBroadcaster.AddListener((fit::SessionMesgListener &)collector);
					mesgBroadcaster.AddListener((fit::LapMesgListener &)collector);
					mesgBroadcaster.AddListener((fit::DeviceInfoMesgListener &)collector);
					mesgBroadcaster.AddListener((fit::EventMesgListener &)collector);
					mesgBroadcaster.AddListener((fit::UserProfileMesgListener &)collector);

					// Decode the file
					decode.Read(&file, &mesgBroadcaster, &mesgBroadcaster, nullptr);

					file.close();

				} catch (const std::exception &e) {
					if (!has_wildcards) {
						// For single files, propagate the error
						throw std::runtime_error("Error reading FIT file '" + file_path + "': " + string(e.what()));
					}
					// For wildcard patterns, continue with next file if one fails
					continue;
				}
			}

			// Copy collected data from all files
			fit_records = std::move(collector.records);
			fit_activities = std::move(collector.activities);
			fit_sessions = std::move(collector.sessions);
			fit_laps = std::move(collector.laps);
			fit_devices = std::move(collector.devices);
			fit_events = std::move(collector.events);
			fit_users = std::move(collector.users);

			// Post-process: populate activity_type from session data
			// This is now done per file, but we keep this for backward compatibility
			if (!fit_sessions.empty() && !fit_records.empty()) {
				// Group records by file source and apply activity types accordingly
				std::map<string, string> file_activity_types;

				// Build mapping of file -> activity type from sessions
				for (const auto &session : fit_sessions) {
					if (!session.sport.empty() && !session.file_source.empty()) {
						file_activity_types[session.file_source] = session.sport;
					}
				}

				// Apply activity types to records based on their file source
				for (auto &record : fit_records) {
					if (file_activity_types.find(record.file_source) != file_activity_types.end()) {
						record.activity_type = file_activity_types[record.file_source];
					}
				}
			}

			// Post-process: populate activities with session data
			if (!fit_sessions.empty() && !fit_activities.empty()) {
				// Group by file source and match sessions to activities
				std::map<string, std::vector<FitSession *>> file_sessions;
				std::map<string, std::vector<FitActivity *>> file_activities;

				for (auto &session : fit_sessions) {
					file_sessions[session.file_source].push_back(&session);
				}
				for (auto &activity : fit_activities) {
					file_activities[activity.file_source].push_back(&activity);
				}

				// Match sessions to activities within each file
				for (const auto &file_pair : file_activities) {
					const string &file_source = file_pair.first;
					auto &activities = file_pair.second;

					if (file_sessions.find(file_source) != file_sessions.end() && !file_sessions[file_source].empty() &&
					    !activities.empty()) {

						auto &session = *file_sessions[file_source][0]; // Use first session
						auto &activity = *activities[0];                // Use first activity

						// Copy sport information
						activity.sport = session.sport;
						activity.sub_sport = session.sub_sport;

						// Copy performance data
						activity.total_distance = session.total_distance;
						activity.total_elapsed_time = session.total_elapsed_time;
						activity.total_calories = session.total_calories;
						activity.avg_heart_rate = session.avg_heart_rate;
						activity.max_heart_rate = session.max_heart_rate;
						activity.avg_speed = session.avg_speed;
						activity.max_speed = session.max_speed;
						activity.avg_power = session.avg_power;
						activity.max_power = session.max_power;
						activity.avg_cadence = session.avg_cadence;
						activity.max_cadence = session.max_cadence;
						activity.total_ascent = session.total_ascent;
						activity.total_descent = session.total_descent;

						// Copy start time if not already set
						if (activity.start_time.value == 0 && session.start_time.value != 0) {
							activity.start_time = session.start_time;
						}
					}
				}
			}

		} catch (const std::exception &e) {
			throw std::runtime_error("Error reading FIT files: " + string(e.what()));
		}
	}
};

static unique_ptr<FunctionData> FitTableBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	// Get the input parameter (file path)
	auto file_path = input.inputs[0].GetValue<string>();

	// Define the FIT data schema with grouped names and types
	struct ColumnDef {
		string name;
		LogicalType type;
	};

	vector<ColumnDef> columns = {// Basic timestamp and location
	                             {"timestamp", LogicalType::TIMESTAMP_TZ},
	                             {"latitude", LogicalType::DOUBLE},
	                             {"longitude", LogicalType::DOUBLE},
	                             {"altitude", LogicalType::DOUBLE},
	                             {"enhanced_altitude", LogicalType::DOUBLE},

	                             // Speed and distance
	                             {"distance", LogicalType::DOUBLE},
	                             {"speed", LogicalType::DOUBLE},
	                             {"enhanced_speed", LogicalType::DOUBLE},
	                             {"vertical_speed", LogicalType::DOUBLE},

	                             // Power metrics
	                             {"power", LogicalType::USMALLINT},                        // uint16_t -> USMALLINT
	                             {"motor_power", LogicalType::USMALLINT},                  // uint16_t -> USMALLINT
	                             {"accumulated_power", LogicalType::UINTEGER},             // uint32_t -> UINTEGER
	                             {"compressed_accumulated_power", LogicalType::USMALLINT}, // uint16_t -> USMALLINT

	                             // Heart rate and physiological data
	                             {"heart_rate", LogicalType::UTINYINT}, // uint8_t -> UTINYINT (corrected)
	                             {"total_hemoglobin_conc", LogicalType::DOUBLE},
	                             {"total_hemoglobin_conc_min", LogicalType::DOUBLE},
	                             {"total_hemoglobin_conc_max", LogicalType::DOUBLE},
	                             {"saturated_hemoglobin_percent", LogicalType::DOUBLE},
	                             {"saturated_hemoglobin_percent_min", LogicalType::DOUBLE},
	                             {"saturated_hemoglobin_percent_max", LogicalType::DOUBLE},

	                             // Cadence metrics
	                             {"cadence", LogicalType::UTINYINT}, // uint8_t -> UTINYINT (corrected)
	                             {"cadence256", LogicalType::DOUBLE},
	                             {"fractional_cadence", LogicalType::DOUBLE},

	                             // Temperature
	                             {"temperature", LogicalType::TINYINT}, // int8_t -> TINYINT (corrected)
	                             {"core_temperature", LogicalType::DOUBLE},

	                             // Cycling metrics
	                             {"grade", LogicalType::DOUBLE},
	                             {"resistance", LogicalType::USMALLINT},        // uint16_t -> USMALLINT
	                             {"left_right_balance", LogicalType::UTINYINT}, // uint8_t -> UTINYINT (corrected)
	                             {"left_torque_effectiveness", LogicalType::DOUBLE},
	                             {"right_torque_effectiveness", LogicalType::DOUBLE},
	                             {"left_pedal_smoothness", LogicalType::DOUBLE},
	                             {"right_pedal_smoothness", LogicalType::DOUBLE},
	                             {"combined_pedal_smoothness", LogicalType::DOUBLE},
	                             {"left_pco", LogicalType::TINYINT},  // int8_t -> TINYINT
	                             {"right_pco", LogicalType::TINYINT}, // int8_t -> TINYINT

	                             // Running metrics
	                             {"vertical_oscillation", LogicalType::DOUBLE},
	                             {"stance_time_percent", LogicalType::DOUBLE},
	                             {"stance_time", LogicalType::DOUBLE},
	                             {"stance_time_balance", LogicalType::DOUBLE},
	                             {"step_length", LogicalType::DOUBLE},
	                             {"vertical_ratio", LogicalType::DOUBLE},

	                             // Cycling/Swimming specific
	                             {"cycle_length", LogicalType::DOUBLE},
	                             {"cycle_length16", LogicalType::DOUBLE},
	                             {"cycles", LogicalType::UTINYINT},
	                             {"total_cycles", LogicalType::UINTEGER}, // uint32_t -> UINTEGER

	                             // Navigation and course
	                             {"time_from_course", LogicalType::DOUBLE},
	                             {"gps_accuracy", LogicalType::UTINYINT},

	                             // Energy and calories
	                             {"calories", LogicalType::USMALLINT}, // uint16_t -> USMALLINT (corrected)

	                             // Zones and training
	                             {"zone", LogicalType::UTINYINT},
	                             {"activity_type", LogicalType::VARCHAR},
	                             {"stroke_type", LogicalType::VARCHAR},

	                             // Advanced metrics
	                             {"time128", LogicalType::DOUBLE},
	                             {"grit", LogicalType::DOUBLE},
	                             {"flow", LogicalType::DOUBLE},
	                             {"current_stress", LogicalType::DOUBLE},

	                             // E-bike specific
	                             {"ebike_travel_range", LogicalType::USMALLINT}, // uint16_t -> USMALLINT
	                             {"ebike_battery_level", LogicalType::UTINYINT},
	                             {"ebike_assist_mode", LogicalType::UTINYINT},
	                             {"ebike_assist_level_percent", LogicalType::UTINYINT},
	                             {"battery_soc", LogicalType::DOUBLE},

	                             // Sports specific
	                             {"ball_speed", LogicalType::DOUBLE},

	                             // Diving/Swimming specific
	                             {"absolute_pressure", LogicalType::UINTEGER}, // uint32_t -> UINTEGER
	                             {"depth", LogicalType::DOUBLE},
	                             {"next_stop_depth", LogicalType::DOUBLE},
	                             {"next_stop_time", LogicalType::UINTEGER},  // uint32_t -> UINTEGER
	                             {"time_to_surface", LogicalType::UINTEGER}, // uint32_t -> UINTEGER
	                             {"ndl_time", LogicalType::UINTEGER},        // uint32_t -> UINTEGER
	                             {"cns_load", LogicalType::UTINYINT},
	                             {"n2_load", LogicalType::USMALLINT},           // uint16_t -> USMALLINT
	                             {"air_time_remaining", LogicalType::UINTEGER}, // uint32_t -> UINTEGER
	                             {"pressure_sac", LogicalType::DOUBLE},
	                             {"volume_sac", LogicalType::DOUBLE},
	                             {"rmv", LogicalType::DOUBLE},
	                             {"ascent_rate", LogicalType::DOUBLE},
	                             {"po2", LogicalType::DOUBLE},

	                             // Respiratory
	                             {"respiration_rate", LogicalType::UTINYINT},
	                             {"enhanced_respiration_rate", LogicalType::DOUBLE},

	                             // Device info
	                             {"device_index", LogicalType::UTINYINT},

	                             // File source
	                             {"file_source", LogicalType::VARCHAR}};

	// Extract names and types from the column definitions
	for (const auto &col : columns) {
		names.push_back(col.name);
		return_types.push_back(col.type);
	}

	return make_uniq<FitTableFunctionData>(file_path, "records", &context);
}

static void FitTableFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = (FitTableFunctionData &)*data_p.bind_data;

	idx_t remaining_rows = data.fit_records.size() - data.current_row;
	idx_t rows_to_output = MinValue<idx_t>(remaining_rows, STANDARD_VECTOR_SIZE);

	if (rows_to_output == 0) {
		output.SetCardinality(0);
		return;
	}

	// Fill the output chunk with FIT data
	for (idx_t row = 0; row < rows_to_output; row++) {
		const auto &fit_record = data.fit_records[data.current_row + row];
		idx_t col = 0;

		// Basic timestamp and location
		output.SetValue(col++, row, Value::TIMESTAMPTZ(fit_record.timestamp));

		output.SetValue(col++, row, fit_record.latitude != 0.0 ? Value::DOUBLE(fit_record.latitude) : Value());
		output.SetValue(col++, row, fit_record.longitude != 0.0 ? Value::DOUBLE(fit_record.longitude) : Value());
		output.SetValue(col++, row, fit_record.altitude != 0.0 ? Value::DOUBLE(fit_record.altitude) : Value());
		output.SetValue(col++, row,
		                fit_record.enhanced_altitude != 0.0 ? Value::DOUBLE(fit_record.enhanced_altitude) : Value());

		// Speed and distance
		output.SetValue(col++, row, fit_record.distance > 0.0 ? Value::DOUBLE(fit_record.distance) : Value());
		output.SetValue(col++, row, fit_record.speed > 0.0 ? Value::DOUBLE(fit_record.speed) : Value());
		output.SetValue(col++, row,
		                fit_record.enhanced_speed > 0.0 ? Value::DOUBLE(fit_record.enhanced_speed) : Value());
		output.SetValue(col++, row,
		                fit_record.vertical_speed != 0.0 ? Value::DOUBLE(fit_record.vertical_speed) : Value());

		// Power metrics - corrected to use proper unsigned types
		output.SetValue(col++, row, fit_record.power > 0 ? Value::USMALLINT(fit_record.power) : Value());
		output.SetValue(col++, row, fit_record.motor_power > 0 ? Value::USMALLINT(fit_record.motor_power) : Value());
		output.SetValue(col++, row,
		                fit_record.accumulated_power > 0 ? Value::UINTEGER(fit_record.accumulated_power) : Value());
		output.SetValue(col++, row,
		                fit_record.compressed_accumulated_power > 0
		                    ? Value::USMALLINT(fit_record.compressed_accumulated_power)
		                    : Value());

		// Heart rate and physiological data - corrected to use UTINYINT for heart_rate
		output.SetValue(col++, row, fit_record.heart_rate > 0 ? Value::UTINYINT(fit_record.heart_rate) : Value());
		output.SetValue(col++, row,
		                fit_record.total_hemoglobin_conc > 0.0 ? Value::DOUBLE(fit_record.total_hemoglobin_conc)
		                                                       : Value());
		output.SetValue(col++, row,
		                fit_record.total_hemoglobin_conc_min > 0.0 ? Value::DOUBLE(fit_record.total_hemoglobin_conc_min)
		                                                           : Value());
		output.SetValue(col++, row,
		                fit_record.total_hemoglobin_conc_max > 0.0 ? Value::DOUBLE(fit_record.total_hemoglobin_conc_max)
		                                                           : Value());
		output.SetValue(col++, row,
		                fit_record.saturated_hemoglobin_percent > 0.0
		                    ? Value::DOUBLE(fit_record.saturated_hemoglobin_percent)
		                    : Value());
		output.SetValue(col++, row,
		                fit_record.saturated_hemoglobin_percent_min > 0.0
		                    ? Value::DOUBLE(fit_record.saturated_hemoglobin_percent_min)
		                    : Value());
		output.SetValue(col++, row,
		                fit_record.saturated_hemoglobin_percent_max > 0.0
		                    ? Value::DOUBLE(fit_record.saturated_hemoglobin_percent_max)
		                    : Value());

		// Cadence metrics - corrected to use UTINYINT for cadence
		output.SetValue(col++, row, fit_record.cadence > 0 ? Value::UTINYINT(fit_record.cadence) : Value());
		output.SetValue(col++, row, fit_record.cadence256 > 0.0 ? Value::DOUBLE(fit_record.cadence256) : Value());
		output.SetValue(col++, row,
		                fit_record.fractional_cadence > 0.0 ? Value::DOUBLE(fit_record.fractional_cadence) : Value());

		// Temperature - corrected to use TINYINT for temperature
		output.SetValue(col++, row, fit_record.temperature != 0 ? Value::TINYINT(fit_record.temperature) : Value());
		output.SetValue(col++, row,
		                fit_record.core_temperature != 0.0 ? Value::DOUBLE(fit_record.core_temperature) : Value());

		// Cycling metrics - corrected types
		output.SetValue(col++, row, fit_record.grade != 0.0 ? Value::DOUBLE(fit_record.grade) : Value());
		output.SetValue(col++, row, fit_record.resistance > 0 ? Value::USMALLINT(fit_record.resistance) : Value());
		output.SetValue(col++, row,
		                fit_record.left_right_balance > 0 ? Value::UTINYINT(fit_record.left_right_balance) : Value());
		output.SetValue(col++, row,
		                fit_record.left_torque_effectiveness != 0.0
		                    ? Value::DOUBLE(fit_record.left_torque_effectiveness)
		                    : Value());
		output.SetValue(col++, row,
		                fit_record.right_torque_effectiveness != 0.0
		                    ? Value::DOUBLE(fit_record.right_torque_effectiveness)
		                    : Value());
		output.SetValue(col++, row,
		                fit_record.left_pedal_smoothness != 0.0 ? Value::DOUBLE(fit_record.left_pedal_smoothness)
		                                                        : Value());
		output.SetValue(col++, row,
		                fit_record.right_pedal_smoothness != 0.0 ? Value::DOUBLE(fit_record.right_pedal_smoothness)
		                                                         : Value());
		output.SetValue(col++, row,
		                fit_record.combined_pedal_smoothness != 0.0
		                    ? Value::DOUBLE(fit_record.combined_pedal_smoothness)
		                    : Value());
		output.SetValue(col++, row, fit_record.left_pco != 0 ? Value::TINYINT(fit_record.left_pco) : Value());
		output.SetValue(col++, row, fit_record.right_pco != 0 ? Value::TINYINT(fit_record.right_pco) : Value());

		// Running metrics
		output.SetValue(col++, row,
		                fit_record.vertical_oscillation > 0.0 ? Value::DOUBLE(fit_record.vertical_oscillation)
		                                                      : Value());
		output.SetValue(col++, row,
		                fit_record.stance_time_percent > 0.0 ? Value::DOUBLE(fit_record.stance_time_percent) : Value());
		output.SetValue(col++, row, fit_record.stance_time > 0.0 ? Value::DOUBLE(fit_record.stance_time) : Value());
		output.SetValue(col++, row,
		                fit_record.stance_time_balance > 0.0 ? Value::DOUBLE(fit_record.stance_time_balance) : Value());
		output.SetValue(col++, row, fit_record.step_length > 0.0 ? Value::DOUBLE(fit_record.step_length) : Value());
		output.SetValue(col++, row,
		                fit_record.vertical_ratio > 0.0 ? Value::DOUBLE(fit_record.vertical_ratio) : Value());

		// Cycling/Swimming specific
		output.SetValue(col++, row, fit_record.cycle_length > 0.0 ? Value::DOUBLE(fit_record.cycle_length) : Value());
		output.SetValue(col++, row,
		                fit_record.cycle_length16 > 0.0 ? Value::DOUBLE(fit_record.cycle_length16) : Value());
		output.SetValue(col++, row, fit_record.cycles > 0 ? Value::UTINYINT(fit_record.cycles) : Value());
		output.SetValue(col++, row, fit_record.total_cycles > 0 ? Value::UINTEGER(fit_record.total_cycles) : Value());

		// Navigation and course
		output.SetValue(col++, row,
		                fit_record.time_from_course != 0.0 ? Value::DOUBLE(fit_record.time_from_course) : Value());
		output.SetValue(col++, row, fit_record.gps_accuracy > 0 ? Value::UTINYINT(fit_record.gps_accuracy) : Value());

		// Energy and calories - corrected to use USMALLINT
		output.SetValue(col++, row, fit_record.calories > 0 ? Value::USMALLINT(fit_record.calories) : Value());

		// Zones and training
		output.SetValue(col++, row, fit_record.zone > 0 ? Value::UTINYINT(fit_record.zone) : Value());
		output.SetValue(col++, row, Value(fit_record.activity_type));
		output.SetValue(col++, row, Value(fit_record.stroke_type));

		// Advanced metrics
		output.SetValue(col++, row, fit_record.time128 != 0.0 ? Value::DOUBLE(fit_record.time128) : Value());
		output.SetValue(col++, row, fit_record.grit != 0.0 ? Value::DOUBLE(fit_record.grit) : Value());
		output.SetValue(col++, row, fit_record.flow != 0.0 ? Value::DOUBLE(fit_record.flow) : Value());
		output.SetValue(col++, row,
		                fit_record.current_stress != 0.0 ? Value::DOUBLE(fit_record.current_stress) : Value());

		// E-bike specific - corrected to use USMALLINT for ebike_travel_range
		output.SetValue(col++, row,
		                fit_record.ebike_travel_range > 0 ? Value::USMALLINT(fit_record.ebike_travel_range) : Value());
		output.SetValue(col++, row,
		                fit_record.ebike_battery_level > 0 ? Value::UTINYINT(fit_record.ebike_battery_level) : Value());
		output.SetValue(col++, row,
		                fit_record.ebike_assist_mode > 0 ? Value::UTINYINT(fit_record.ebike_assist_mode) : Value());
		output.SetValue(col++, row,
		                fit_record.ebike_assist_level_percent > 0
		                    ? Value::UTINYINT(fit_record.ebike_assist_level_percent)
		                    : Value());
		output.SetValue(col++, row, fit_record.battery_soc > 0.0 ? Value::DOUBLE(fit_record.battery_soc) : Value());

		// Sports specific
		output.SetValue(col++, row, fit_record.ball_speed > 0.0 ? Value::DOUBLE(fit_record.ball_speed) : Value());

		// Diving/Swimming specific - corrected to use proper unsigned types
		output.SetValue(col++, row,
		                fit_record.absolute_pressure > 0 ? Value::UINTEGER(fit_record.absolute_pressure) : Value());
		output.SetValue(col++, row, fit_record.depth > 0.0 ? Value::DOUBLE(fit_record.depth) : Value());
		output.SetValue(col++, row,
		                fit_record.next_stop_depth > 0.0 ? Value::DOUBLE(fit_record.next_stop_depth) : Value());
		output.SetValue(col++, row,
		                fit_record.next_stop_time > 0 ? Value::UINTEGER(fit_record.next_stop_time) : Value());
		output.SetValue(col++, row,
		                fit_record.time_to_surface > 0 ? Value::UINTEGER(fit_record.time_to_surface) : Value());
		output.SetValue(col++, row, fit_record.ndl_time > 0 ? Value::UINTEGER(fit_record.ndl_time) : Value());
		output.SetValue(col++, row, fit_record.cns_load > 0 ? Value::UTINYINT(fit_record.cns_load) : Value());
		output.SetValue(col++, row, fit_record.n2_load > 0 ? Value::USMALLINT(fit_record.n2_load) : Value());
		output.SetValue(col++, row,
		                fit_record.air_time_remaining > 0 ? Value::UINTEGER(fit_record.air_time_remaining) : Value());
		output.SetValue(col++, row, fit_record.pressure_sac > 0.0 ? Value::DOUBLE(fit_record.pressure_sac) : Value());
		output.SetValue(col++, row, fit_record.volume_sac > 0.0 ? Value::DOUBLE(fit_record.volume_sac) : Value());
		output.SetValue(col++, row, fit_record.rmv > 0.0 ? Value::DOUBLE(fit_record.rmv) : Value());
		output.SetValue(col++, row, fit_record.ascent_rate != 0.0 ? Value::DOUBLE(fit_record.ascent_rate) : Value());
		output.SetValue(col++, row, fit_record.po2 > 0.0 ? Value::DOUBLE(fit_record.po2) : Value());

		// Respiratory
		output.SetValue(col++, row,
		                fit_record.respiration_rate > 0 ? Value::UTINYINT(fit_record.respiration_rate) : Value());
		output.SetValue(col++, row,
		                fit_record.enhanced_respiration_rate > 0.0 ? Value::DOUBLE(fit_record.enhanced_respiration_rate)
		                                                           : Value());

		// Device info
		output.SetValue(col++, row, fit_record.device_index > 0 ? Value::UTINYINT(fit_record.device_index) : Value());

		// File source
		output.SetValue(col++, row, !fit_record.file_source.empty() ? Value(fit_record.file_source) : Value());
	}

	output.SetCardinality(rows_to_output);
	data.current_row += rows_to_output;
}

// ===== FIT ACTIVITIES TABLE FUNCTION =====
static unique_ptr<FunctionData> FitActivitiesBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto file_path = input.inputs[0].GetValue<string>();

	// Define schema for activities metadata table
	vector<pair<string, LogicalType>> columns = {{"activity_id", LogicalType::UBIGINT},
	                                             {"file_id", LogicalType::VARCHAR},
	                                             {"timestamp", LogicalType::TIMESTAMP_TZ},
	                                             {"local_timestamp", LogicalType::TIMESTAMP_TZ},
	                                             {"start_time", LogicalType::TIMESTAMP_TZ},
	                                             {"total_timer_time", LogicalType::DOUBLE},
	                                             {"total_elapsed_time", LogicalType::DOUBLE},
	                                             {"total_distance", LogicalType::DOUBLE},
	                                             {"sport", LogicalType::VARCHAR},
	                                             {"sub_sport", LogicalType::VARCHAR},
	                                             {"manufacturer", LogicalType::VARCHAR},
	                                             {"product", LogicalType::VARCHAR},
	                                             {"device_serial_number", LogicalType::UBIGINT},
	                                             {"software_version", LogicalType::VARCHAR},
	                                             {"total_calories", LogicalType::UINTEGER},
	                                             {"total_ascent", LogicalType::DOUBLE},
	                                             {"total_descent", LogicalType::DOUBLE},
	                                             {"avg_heart_rate", LogicalType::UTINYINT},
	                                             {"max_heart_rate", LogicalType::UTINYINT},
	                                             {"avg_speed", LogicalType::DOUBLE},
	                                             {"max_speed", LogicalType::DOUBLE},
	                                             {"avg_power", LogicalType::USMALLINT},
	                                             {"max_power", LogicalType::USMALLINT},
	                                             {"avg_cadence", LogicalType::UTINYINT},
	                                             {"max_cadence", LogicalType::UTINYINT},
	                                             {"start_position_lat", LogicalType::DOUBLE},
	                                             {"start_position_long", LogicalType::DOUBLE},
	                                             {"end_position_lat", LogicalType::DOUBLE},
	                                             {"end_position_long", LogicalType::DOUBLE},
	                                             {"file_source", LogicalType::VARCHAR}};

	for (const auto &col : columns) {
		names.push_back(col.first);
		return_types.push_back(col.second);
	}

	return make_uniq<FitTableFunctionData>(file_path, "activities", &context);
}

static void FitActivitiesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = (FitTableFunctionData &)*data_p.bind_data;

	idx_t remaining_rows = data.fit_activities.size() - data.current_row;
	idx_t rows_to_output = MinValue<idx_t>(remaining_rows, STANDARD_VECTOR_SIZE);

	if (rows_to_output == 0) {
		output.SetCardinality(0);
		return;
	}

	for (idx_t row = 0; row < rows_to_output; row++) {
		const auto &activity = data.fit_activities[data.current_row + row];
		idx_t col = 0;

		output.SetValue(col++, row, Value::UBIGINT(activity.activity_id));
		output.SetValue(col++, row, Value(activity.file_id));
		output.SetValue(col++, row, Value::TIMESTAMPTZ(activity.timestamp));
		output.SetValue(col++, row, Value::TIMESTAMPTZ(activity.local_timestamp));
		output.SetValue(col++, row, Value::TIMESTAMPTZ(activity.start_time));
		output.SetValue(col++, row, Value::DOUBLE(activity.total_timer_time));
		output.SetValue(col++, row, Value::DOUBLE(activity.total_elapsed_time));
		output.SetValue(col++, row, Value::DOUBLE(activity.total_distance));
		output.SetValue(col++, row, Value(activity.sport));
		output.SetValue(col++, row, Value(activity.sub_sport));
		output.SetValue(col++, row, Value(activity.manufacturer));
		output.SetValue(col++, row, Value(activity.product));
		output.SetValue(col++, row, Value::UBIGINT(activity.device_serial_number));
		output.SetValue(col++, row, Value(activity.software_version));
		output.SetValue(col++, row, Value::UINTEGER(activity.total_calories));
		output.SetValue(col++, row, Value::DOUBLE(activity.total_ascent));
		output.SetValue(col++, row, Value::DOUBLE(activity.total_descent));
		output.SetValue(col++, row, activity.avg_heart_rate > 0 ? Value::UTINYINT(activity.avg_heart_rate) : Value());
		output.SetValue(col++, row, activity.max_heart_rate > 0 ? Value::UTINYINT(activity.max_heart_rate) : Value());
		output.SetValue(col++, row, Value::DOUBLE(activity.avg_speed));
		output.SetValue(col++, row, Value::DOUBLE(activity.max_speed));
		output.SetValue(col++, row, activity.avg_power > 0 ? Value::USMALLINT(activity.avg_power) : Value());
		output.SetValue(col++, row, activity.max_power > 0 ? Value::USMALLINT(activity.max_power) : Value());
		output.SetValue(col++, row, activity.avg_cadence > 0 ? Value::UTINYINT(activity.avg_cadence) : Value());
		output.SetValue(col++, row, activity.max_cadence > 0 ? Value::UTINYINT(activity.max_cadence) : Value());
		output.SetValue(col++, row,
		                activity.start_position_lat != 0.0 ? Value::DOUBLE(activity.start_position_lat) : Value());
		output.SetValue(col++, row,
		                activity.start_position_long != 0.0 ? Value::DOUBLE(activity.start_position_long) : Value());
		output.SetValue(col++, row,
		                activity.end_position_lat != 0.0 ? Value::DOUBLE(activity.end_position_lat) : Value());
		output.SetValue(col++, row,
		                activity.end_position_long != 0.0 ? Value::DOUBLE(activity.end_position_long) : Value());
		output.SetValue(col++, row, !activity.file_source.empty() ? Value(activity.file_source) : Value());
	}

	output.SetCardinality(rows_to_output);
	data.current_row += rows_to_output;
}

// (Additional table functions would go here - continuing in next message due to length...)

// ===== FIT SESSIONS TABLE FUNCTION =====
static unique_ptr<FunctionData> FitSessionsBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto file_path = input.inputs[0].GetValue<string>();

	vector<pair<string, LogicalType>> columns = {
	    {"session_id", LogicalType::UINTEGER},       {"activity_id", LogicalType::UBIGINT},
	    {"timestamp", LogicalType::TIMESTAMP_TZ},    {"start_time", LogicalType::TIMESTAMP_TZ},
	    {"total_elapsed_time", LogicalType::DOUBLE}, {"total_timer_time", LogicalType::DOUBLE},
	    {"total_distance", LogicalType::DOUBLE},     {"sport", LogicalType::VARCHAR},
	    {"sub_sport", LogicalType::VARCHAR},         {"total_calories", LogicalType::UINTEGER},
	    {"avg_speed", LogicalType::DOUBLE},          {"max_speed", LogicalType::DOUBLE},
	    {"avg_heart_rate", LogicalType::UTINYINT},   {"max_heart_rate", LogicalType::UTINYINT},
	    {"min_heart_rate", LogicalType::UTINYINT},   {"avg_cadence", LogicalType::UTINYINT},
	    {"max_cadence", LogicalType::UTINYINT},      {"avg_power", LogicalType::USMALLINT},
	    {"max_power", LogicalType::USMALLINT},       {"normalized_power", LogicalType::USMALLINT},
	    {"intensity_factor", LogicalType::DOUBLE},   {"training_stress_score", LogicalType::DOUBLE},
	    {"total_work", LogicalType::UINTEGER},       {"total_ascent", LogicalType::DOUBLE},
	    {"total_descent", LogicalType::DOUBLE},      {"first_lap_index", LogicalType::UTINYINT},
	    {"num_laps", LogicalType::UTINYINT},         {"event", LogicalType::VARCHAR},
	    {"event_type", LogicalType::VARCHAR},        {"trigger", LogicalType::VARCHAR},
	    {"file_source", LogicalType::VARCHAR}};

	for (const auto &col : columns) {
		names.push_back(col.first);
		return_types.push_back(col.second);
	}

	return make_uniq<FitTableFunctionData>(file_path, "sessions", &context);
}

static void FitSessionsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = (FitTableFunctionData &)*data_p.bind_data;

	idx_t remaining_rows = data.fit_sessions.size() - data.current_row;
	idx_t rows_to_output = MinValue<idx_t>(remaining_rows, STANDARD_VECTOR_SIZE);

	if (rows_to_output == 0) {
		output.SetCardinality(0);
		return;
	}

	for (idx_t row = 0; row < rows_to_output; row++) {
		const auto &session = data.fit_sessions[data.current_row + row];
		idx_t col = 0;

		output.SetValue(col++, row, Value::UINTEGER(session.session_id));
		output.SetValue(col++, row, Value::UBIGINT(session.activity_id));
		output.SetValue(col++, row, Value::TIMESTAMPTZ(session.timestamp));
		output.SetValue(col++, row, Value::TIMESTAMPTZ(session.start_time));
		output.SetValue(col++, row, Value::DOUBLE(session.total_elapsed_time));
		output.SetValue(col++, row, Value::DOUBLE(session.total_timer_time));
		output.SetValue(col++, row, Value::DOUBLE(session.total_distance));
		output.SetValue(col++, row, Value(session.sport));
		output.SetValue(col++, row, Value(session.sub_sport));
		output.SetValue(col++, row, Value::UINTEGER(session.total_calories));
		output.SetValue(col++, row, Value::DOUBLE(session.avg_speed));
		output.SetValue(col++, row, Value::DOUBLE(session.max_speed));
		output.SetValue(col++, row, session.avg_heart_rate > 0 ? Value::UTINYINT(session.avg_heart_rate) : Value());
		output.SetValue(col++, row, session.max_heart_rate > 0 ? Value::UTINYINT(session.max_heart_rate) : Value());
		output.SetValue(col++, row, session.min_heart_rate > 0 ? Value::UTINYINT(session.min_heart_rate) : Value());
		output.SetValue(col++, row, session.avg_cadence > 0 ? Value::UTINYINT(session.avg_cadence) : Value());
		output.SetValue(col++, row, session.max_cadence > 0 ? Value::UTINYINT(session.max_cadence) : Value());
		output.SetValue(col++, row, session.avg_power > 0 ? Value::USMALLINT(session.avg_power) : Value());
		output.SetValue(col++, row, session.max_power > 0 ? Value::USMALLINT(session.max_power) : Value());
		output.SetValue(col++, row,
		                session.normalized_power > 0 ? Value::USMALLINT(session.normalized_power) : Value());
		output.SetValue(col++, row, Value::DOUBLE(session.intensity_factor));
		output.SetValue(col++, row, Value::DOUBLE(session.training_stress_score));
		output.SetValue(col++, row, Value::UINTEGER(session.total_work));
		output.SetValue(col++, row, Value::DOUBLE(session.total_ascent));
		output.SetValue(col++, row, Value::DOUBLE(session.total_descent));
		output.SetValue(col++, row, Value::UTINYINT(session.first_lap_index));
		output.SetValue(col++, row, Value::UTINYINT(session.num_laps));
		output.SetValue(col++, row, Value(session.event));
		output.SetValue(col++, row, Value(session.event_type));
		output.SetValue(col++, row, Value(session.trigger));
	}

	output.SetCardinality(rows_to_output);
	data.current_row += rows_to_output;
}

// ===== FIT LAPS TABLE FUNCTION =====
static unique_ptr<FunctionData> FitLapsBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto file_path = input.inputs[0].GetValue<string>();

	// Define lap columns based on FitLap structure
	names = {"lap_id",
	         "session_id",
	         "activity_id",
	         "timestamp",
	         "start_time",
	         "total_elapsed_time",
	         "total_timer_time",
	         "total_distance",
	         "total_calories",
	         "avg_speed",
	         "max_speed",
	         "avg_heart_rate",
	         "max_heart_rate",
	         "min_heart_rate",
	         "avg_cadence",
	         "max_cadence",
	         "avg_power",
	         "max_power",
	         "total_ascent",
	         "total_descent",
	         "lap_trigger",
	         "event",
	         "event_type",
	         "start_position_lat",
	         "start_position_long",
	         "end_position_lat",
	         "end_position_long",
	         "file_source"};

	return_types = {LogicalType::UINTEGER,     LogicalType::UINTEGER,  LogicalType::UBIGINT,  LogicalType::TIMESTAMP_TZ,
	                LogicalType::TIMESTAMP_TZ, LogicalType::DOUBLE,    LogicalType::DOUBLE,   LogicalType::DOUBLE,
	                LogicalType::UINTEGER,     LogicalType::DOUBLE,    LogicalType::DOUBLE,   LogicalType::UTINYINT,
	                LogicalType::UTINYINT,     LogicalType::UTINYINT,  LogicalType::UTINYINT, LogicalType::UTINYINT,
	                LogicalType::USMALLINT,    LogicalType::USMALLINT, LogicalType::DOUBLE,   LogicalType::DOUBLE,
	                LogicalType::VARCHAR,      LogicalType::VARCHAR,   LogicalType::VARCHAR,  LogicalType::DOUBLE,
	                LogicalType::DOUBLE,       LogicalType::DOUBLE,    LogicalType::DOUBLE,   LogicalType::VARCHAR};

	return make_uniq<FitTableFunctionData>(file_path, "laps", &context);
}

static void FitLapsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = (FitTableFunctionData &)*data_p.bind_data;

	idx_t remaining_rows = data.fit_laps.size() - data.current_row;
	idx_t rows_to_output = MinValue<idx_t>(remaining_rows, STANDARD_VECTOR_SIZE);

	if (rows_to_output == 0) {
		output.SetCardinality(0);
		return;
	}

	// Fill the output chunk with lap data
	for (idx_t row = 0; row < rows_to_output; row++) {
		const auto &fit_lap = data.fit_laps[data.current_row + row];
		idx_t col = 0;

		output.SetValue(col++, row, Value::UINTEGER(fit_lap.lap_id));
		output.SetValue(col++, row, Value::UINTEGER(fit_lap.session_id));
		output.SetValue(col++, row, Value::UBIGINT(fit_lap.activity_id));
		output.SetValue(col++, row, Value::TIMESTAMPTZ(fit_lap.timestamp));
		output.SetValue(col++, row, Value::TIMESTAMPTZ(fit_lap.start_time));
		output.SetValue(col++, row,
		                fit_lap.total_elapsed_time > 0.0 ? Value::DOUBLE(fit_lap.total_elapsed_time) : Value());
		output.SetValue(col++, row, fit_lap.total_timer_time > 0.0 ? Value::DOUBLE(fit_lap.total_timer_time) : Value());
		output.SetValue(col++, row, fit_lap.total_distance > 0.0 ? Value::DOUBLE(fit_lap.total_distance) : Value());
		output.SetValue(col++, row, fit_lap.total_calories > 0 ? Value::UINTEGER(fit_lap.total_calories) : Value());
		output.SetValue(col++, row, fit_lap.avg_speed > 0.0 ? Value::DOUBLE(fit_lap.avg_speed) : Value());
		output.SetValue(col++, row, fit_lap.max_speed > 0.0 ? Value::DOUBLE(fit_lap.max_speed) : Value());
		output.SetValue(col++, row, fit_lap.avg_heart_rate > 0 ? Value::UTINYINT(fit_lap.avg_heart_rate) : Value());
		output.SetValue(col++, row, fit_lap.max_heart_rate > 0 ? Value::UTINYINT(fit_lap.max_heart_rate) : Value());
		output.SetValue(col++, row, fit_lap.min_heart_rate > 0 ? Value::UTINYINT(fit_lap.min_heart_rate) : Value());
		output.SetValue(col++, row, fit_lap.avg_cadence > 0 ? Value::UTINYINT(fit_lap.avg_cadence) : Value());
		output.SetValue(col++, row, fit_lap.max_cadence > 0 ? Value::UTINYINT(fit_lap.max_cadence) : Value());
		output.SetValue(col++, row, fit_lap.avg_power > 0 ? Value::USMALLINT(fit_lap.avg_power) : Value());
		output.SetValue(col++, row, fit_lap.max_power > 0 ? Value::USMALLINT(fit_lap.max_power) : Value());
		output.SetValue(col++, row, fit_lap.total_ascent > 0.0 ? Value::DOUBLE(fit_lap.total_ascent) : Value());
		output.SetValue(col++, row, fit_lap.total_descent > 0.0 ? Value::DOUBLE(fit_lap.total_descent) : Value());
		output.SetValue(col++, row, Value(fit_lap.lap_trigger));
		output.SetValue(col++, row, Value(fit_lap.event));
		output.SetValue(col++, row, Value(fit_lap.event_type));
		output.SetValue(col++, row,
		                fit_lap.start_position_lat != 0.0 ? Value::DOUBLE(fit_lap.start_position_lat) : Value());
		output.SetValue(col++, row,
		                fit_lap.start_position_long != 0.0 ? Value::DOUBLE(fit_lap.start_position_long) : Value());
		output.SetValue(col++, row,
		                fit_lap.end_position_lat != 0.0 ? Value::DOUBLE(fit_lap.end_position_lat) : Value());
		output.SetValue(col++, row,
		                fit_lap.end_position_long != 0.0 ? Value::DOUBLE(fit_lap.end_position_long) : Value());
	}

	output.SetCardinality(rows_to_output);
	data.current_row += rows_to_output;
}

// ===== FIT DEVICES TABLE FUNCTION =====
static unique_ptr<FunctionData> FitDevicesBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	auto file_path = input.inputs[0].GetValue<string>();

	// Define device columns based on FitDevice structure
	names = {"device_id",      "activity_id",     "device_index",     "device_type",           "manufacturer",
	         "product",        "serial_number",   "software_version", "hardware_version",      "cum_operating_time",
	         "battery_status", "sensor_position", "descriptor",       "ant_transmission_type", "ant_device_number",
	         "ant_network",    "source_type",     "product_name",     "battery_voltage",       "file_source"};

	return_types = {LogicalType::UINTEGER, LogicalType::UBIGINT,  LogicalType::UTINYINT,  LogicalType::VARCHAR,
	                LogicalType::VARCHAR,  LogicalType::VARCHAR,  LogicalType::UBIGINT,   LogicalType::VARCHAR,
	                LogicalType::VARCHAR,  LogicalType::UINTEGER, LogicalType::VARCHAR,   LogicalType::VARCHAR,
	                LogicalType::VARCHAR,  LogicalType::UTINYINT, LogicalType::USMALLINT, LogicalType::VARCHAR,
	                LogicalType::VARCHAR,  LogicalType::VARCHAR,  LogicalType::DOUBLE,    LogicalType::VARCHAR};

	return make_uniq<FitTableFunctionData>(file_path, "devices", &context);
}

static void FitDevicesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = (FitTableFunctionData &)*data_p.bind_data;

	idx_t remaining_rows = data.fit_devices.size() - data.current_row;
	idx_t rows_to_output = MinValue<idx_t>(remaining_rows, STANDARD_VECTOR_SIZE);

	if (rows_to_output == 0) {
		output.SetCardinality(0);
		return;
	}

	// Fill the output chunk with device data
	for (idx_t row = 0; row < rows_to_output; row++) {
		const auto &fit_device = data.fit_devices[data.current_row + row];
		idx_t col = 0;

		output.SetValue(col++, row, Value::UINTEGER(fit_device.device_id));
		output.SetValue(col++, row, Value::UBIGINT(fit_device.activity_id));
		output.SetValue(col++, row, Value::UTINYINT(fit_device.device_index));
		output.SetValue(col++, row, Value(fit_device.device_type));
		output.SetValue(col++, row, Value(fit_device.manufacturer));
		output.SetValue(col++, row, Value(fit_device.product));
		output.SetValue(col++, row, fit_device.serial_number > 0 ? Value::UBIGINT(fit_device.serial_number) : Value());
		output.SetValue(col++, row, Value(fit_device.software_version));
		output.SetValue(col++, row, Value(fit_device.hardware_version));
		output.SetValue(col++, row,
		                fit_device.cum_operating_time > 0 ? Value::UINTEGER(fit_device.cum_operating_time) : Value());
		output.SetValue(col++, row, Value(fit_device.battery_status));
		output.SetValue(col++, row, Value(fit_device.sensor_position));
		output.SetValue(col++, row, Value(fit_device.descriptor));
		output.SetValue(col++, row,
		                fit_device.ant_transmission_type > 0 ? Value::UTINYINT(fit_device.ant_transmission_type)
		                                                     : Value());
		output.SetValue(col++, row,
		                fit_device.ant_device_number > 0 ? Value::USMALLINT(fit_device.ant_device_number) : Value());
		output.SetValue(col++, row, Value(fit_device.ant_network));
		output.SetValue(col++, row, Value(fit_device.source_type));
		output.SetValue(col++, row, Value(fit_device.product_name));
		output.SetValue(col++, row,
		                fit_device.battery_voltage > 0.0 ? Value::DOUBLE(fit_device.battery_voltage) : Value());
	}

	output.SetCardinality(rows_to_output);
	data.current_row += rows_to_output;
}

// ===== FIT EVENTS TABLE FUNCTION =====
static unique_ptr<FunctionData> FitEventsBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto file_path = input.inputs[0].GetValue<string>();

	// Define event columns based on FitEvent structure
	names = {"event_id",  "activity_id",  "timestamp",      "event",           "event_type", "data",
	         "data16",    "score",        "opponent_score", "front_gear_num",  "front_gear", "rear_gear_num",
	         "rear_gear", "device_index", "activity_type",  "start_timestamp", "file_source"};

	return_types = {LogicalType::UINTEGER,  LogicalType::UBIGINT,  LogicalType::TIMESTAMP_TZ, LogicalType::VARCHAR,
	                LogicalType::VARCHAR,   LogicalType::UINTEGER, LogicalType::USMALLINT,    LogicalType::USMALLINT,
	                LogicalType::USMALLINT, LogicalType::UTINYINT, LogicalType::UTINYINT,     LogicalType::UTINYINT,
	                LogicalType::UTINYINT,  LogicalType::UTINYINT, LogicalType::VARCHAR,      LogicalType::TIMESTAMP_TZ,
	                LogicalType::VARCHAR};

	return make_uniq<FitTableFunctionData>(file_path, "events", &context);
}

static void FitEventsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = (FitTableFunctionData &)*data_p.bind_data;

	idx_t remaining_rows = data.fit_events.size() - data.current_row;
	idx_t rows_to_output = MinValue<idx_t>(remaining_rows, STANDARD_VECTOR_SIZE);

	if (rows_to_output == 0) {
		output.SetCardinality(0);
		return;
	}

	// Fill the output chunk with event data
	for (idx_t row = 0; row < rows_to_output; row++) {
		const auto &fit_event = data.fit_events[data.current_row + row];
		idx_t col = 0;

		output.SetValue(col++, row, Value::UINTEGER(fit_event.event_id));
		output.SetValue(col++, row, Value::UBIGINT(fit_event.activity_id));
		output.SetValue(col++, row, Value::TIMESTAMPTZ(fit_event.timestamp));
		output.SetValue(col++, row, Value(fit_event.event));
		output.SetValue(col++, row, Value(fit_event.event_type));
		output.SetValue(col++, row, fit_event.data > 0 ? Value::UINTEGER(fit_event.data) : Value());
		output.SetValue(col++, row, fit_event.data16 > 0 ? Value::USMALLINT(fit_event.data16) : Value());
		output.SetValue(col++, row, fit_event.score > 0 ? Value::USMALLINT(fit_event.score) : Value());
		output.SetValue(col++, row,
		                fit_event.opponent_score > 0 ? Value::USMALLINT(fit_event.opponent_score) : Value());
		output.SetValue(col++, row, fit_event.front_gear_num > 0 ? Value::UTINYINT(fit_event.front_gear_num) : Value());
		output.SetValue(col++, row, fit_event.front_gear > 0 ? Value::UTINYINT(fit_event.front_gear) : Value());
		output.SetValue(col++, row, fit_event.rear_gear_num > 0 ? Value::UTINYINT(fit_event.rear_gear_num) : Value());
		output.SetValue(col++, row, fit_event.rear_gear > 0 ? Value::UTINYINT(fit_event.rear_gear) : Value());
		output.SetValue(col++, row, Value::UTINYINT(fit_event.device_index));
		output.SetValue(col++, row, Value(fit_event.activity_type));
		output.SetValue(col++, row, Value::TIMESTAMPTZ(fit_event.start_timestamp));
	}

	output.SetCardinality(rows_to_output);
	data.current_row += rows_to_output;
}

// ===== FIT USERS TABLE FUNCTION =====
static unique_ptr<FunctionData> FitUsersBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto file_path = input.inputs[0].GetValue<string>();

	// Define user columns based on FitUser structure
	names = {"user_id",
	         "gender",
	         "age",
	         "height",
	         "weight",
	         "language",
	         "time_zone",
	         "activity_class",
	         "running_lactate_threshold_hr",
	         "cycling_lactate_threshold_hr",
	         "swimming_lactate_threshold_hr",
	         "default_max_running_hr",
	         "default_max_biking_hr",
	         "default_max_hr",
	         "hr_setting",
	         "speed_setting",
	         "dist_setting",
	         "power_setting",
	         "position_setting",
	         "temperature_setting",
	         "local_id",
	         "global_id",
	         "wake_time",
	         "sleep_time",
	         "height_setting",
	         "weight_setting",
	         "resting_heart_rate",
	         "default_max_swimming_hr",
	         "file_source"};

	return_types = {LogicalType::UINTEGER, LogicalType::VARCHAR,  LogicalType::UTINYINT, LogicalType::DOUBLE,
	                LogicalType::DOUBLE,   LogicalType::VARCHAR,  LogicalType::TINYINT,  LogicalType::DOUBLE,
	                LogicalType::UTINYINT, LogicalType::UTINYINT, LogicalType::UTINYINT, LogicalType::UTINYINT,
	                LogicalType::UTINYINT, LogicalType::UTINYINT, LogicalType::VARCHAR,  LogicalType::VARCHAR,
	                LogicalType::VARCHAR,  LogicalType::VARCHAR,  LogicalType::VARCHAR,  LogicalType::VARCHAR,
	                LogicalType::UINTEGER, LogicalType::UBIGINT,  LogicalType::UINTEGER, LogicalType::UINTEGER,
	                LogicalType::VARCHAR,  LogicalType::VARCHAR,  LogicalType::UTINYINT, LogicalType::UTINYINT,
	                LogicalType::VARCHAR};

	return make_uniq<FitTableFunctionData>(file_path, "users", &context);
}

static void FitUsersFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = (FitTableFunctionData &)*data_p.bind_data;

	idx_t remaining_rows = data.fit_users.size() - data.current_row;
	idx_t rows_to_output = MinValue<idx_t>(remaining_rows, STANDARD_VECTOR_SIZE);

	if (rows_to_output == 0) {
		output.SetCardinality(0);
		return;
	}

	// Fill the output chunk with user data
	for (idx_t row = 0; row < rows_to_output; row++) {
		const auto &fit_user = data.fit_users[data.current_row + row];
		idx_t col = 0;

		output.SetValue(col++, row, Value::UINTEGER(fit_user.user_id));
		output.SetValue(col++, row, Value(fit_user.gender));
		output.SetValue(col++, row, fit_user.age > 0 ? Value::UTINYINT(fit_user.age) : Value());
		output.SetValue(col++, row, fit_user.height > 0.0 ? Value::DOUBLE(fit_user.height) : Value());
		output.SetValue(col++, row, fit_user.weight > 0.0 ? Value::DOUBLE(fit_user.weight) : Value());
		output.SetValue(col++, row, Value(fit_user.language));
		output.SetValue(col++, row, Value::TINYINT(fit_user.time_zone));
		output.SetValue(col++, row, fit_user.activity_class > 0.0 ? Value::DOUBLE(fit_user.activity_class) : Value());
		output.SetValue(col++, row,
		                fit_user.running_lactate_threshold_hr > 0
		                    ? Value::UTINYINT(fit_user.running_lactate_threshold_hr)
		                    : Value());
		output.SetValue(col++, row,
		                fit_user.cycling_lactate_threshold_hr > 0
		                    ? Value::UTINYINT(fit_user.cycling_lactate_threshold_hr)
		                    : Value());
		output.SetValue(col++, row,
		                fit_user.swimming_lactate_threshold_hr > 0
		                    ? Value::UTINYINT(fit_user.swimming_lactate_threshold_hr)
		                    : Value());
		output.SetValue(col++, row,
		                fit_user.default_max_running_hr > 0 ? Value::UTINYINT(fit_user.default_max_running_hr)
		                                                    : Value());
		output.SetValue(col++, row,
		                fit_user.default_max_biking_hr > 0 ? Value::UTINYINT(fit_user.default_max_biking_hr) : Value());
		output.SetValue(col++, row, fit_user.default_max_hr > 0 ? Value::UTINYINT(fit_user.default_max_hr) : Value());
		output.SetValue(col++, row, Value(fit_user.hr_setting));
		output.SetValue(col++, row, Value(fit_user.speed_setting));
		output.SetValue(col++, row, Value(fit_user.dist_setting));
		output.SetValue(col++, row, Value(fit_user.power_setting));
		output.SetValue(col++, row, Value(fit_user.position_setting));
		output.SetValue(col++, row, Value(fit_user.temperature_setting));
		output.SetValue(col++, row, fit_user.local_id > 0 ? Value::UINTEGER(fit_user.local_id) : Value());
		output.SetValue(col++, row, fit_user.global_id > 0 ? Value::UBIGINT(fit_user.global_id) : Value());
		output.SetValue(col++, row, fit_user.wake_time > 0 ? Value::UINTEGER(fit_user.wake_time) : Value());
		output.SetValue(col++, row, fit_user.sleep_time > 0 ? Value::UINTEGER(fit_user.sleep_time) : Value());
		output.SetValue(col++, row, Value(fit_user.height_setting));
		output.SetValue(col++, row, Value(fit_user.weight_setting));
		output.SetValue(col++, row,
		                fit_user.resting_heart_rate > 0 ? Value::UTINYINT(fit_user.resting_heart_rate) : Value());
		output.SetValue(col++, row,
		                fit_user.default_max_swimming_hr > 0 ? Value::UTINYINT(fit_user.default_max_swimming_hr)
		                                                     : Value());
	}

	output.SetCardinality(rows_to_output);
	data.current_row += rows_to_output;
}

inline void FitOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Fit " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register the 7 FIT table functions corresponding to the 7 tables in documentation

	// 1. Time-series records table (original 'fit' function)
	TableFunction fit_records_function("fit_records", {LogicalType::VARCHAR}, FitTableFunction, FitTableBind);
	loader.RegisterFunction(fit_records_function);

	// Keep original 'fit' function name for backward compatibility
	TableFunction fit_table_function("fit", {LogicalType::VARCHAR}, FitTableFunction, FitTableBind);
	loader.RegisterFunction(fit_table_function);

	// 2. Activities metadata table
	TableFunction fit_activities_function("fit_activities", {LogicalType::VARCHAR}, FitActivitiesFunction,
	                                      FitActivitiesBind);
	loader.RegisterFunction(fit_activities_function);

	// 3. Sessions table
	TableFunction fit_sessions_function("fit_sessions", {LogicalType::VARCHAR}, FitSessionsFunction, FitSessionsBind);
	loader.RegisterFunction(fit_sessions_function);

	// 4. Laps table
	TableFunction fit_laps_function("fit_laps", {LogicalType::VARCHAR}, FitLapsFunction, FitLapsBind);
	loader.RegisterFunction(fit_laps_function);

	// 5. Device information table
	TableFunction fit_devices_function("fit_devices", {LogicalType::VARCHAR}, FitDevicesFunction, FitDevicesBind);
	loader.RegisterFunction(fit_devices_function);

	// 6. Events table
	TableFunction fit_events_function("fit_events", {LogicalType::VARCHAR}, FitEventsFunction, FitEventsBind);
	loader.RegisterFunction(fit_events_function);

	// 7. User profile table
	TableFunction fit_users_function("fit_users", {LogicalType::VARCHAR}, FitUsersFunction, FitUsersBind);
	loader.RegisterFunction(fit_users_function);

	// Register scalar function
	auto fit_openssl_version_scalar_function =
	    ScalarFunction("fit_openssl_version", {LogicalType::VARCHAR}, LogicalType::VARCHAR, FitOpenSSLVersionScalarFun);
	loader.RegisterFunction(fit_openssl_version_scalar_function);
}

void FitExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string FitExtension::Name() {
	return "fit";
}

std::string FitExtension::Version() const {
#ifdef EXT_VERSION_FIT
	return EXT_VERSION_FIT;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(fit, loader) {
	duckdb::LoadInternal(loader);
}
}
