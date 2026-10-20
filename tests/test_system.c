#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "tests.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/system.h"

enum {
	SECONDS_PER_MINUTE = 60,
	MINUTES_PER_HOUR = 60,
	HOURS_PER_DAY = 24,
};

static bool read_setting(uint16_t setting, void *value, uint16_t value_bytes)
{
	uint16_t required_bytes;
	return NT_SUCCESS(ExGetXConfigSetting(XCONFIG_USER_CATEGORY, setting, value, value_bytes, &required_bytes));
}

// How far local time is ahead of UTC, from the two broken down, which are a day apart at most.
static long seconds_ahead(const struct tm *local, const struct tm *utc)
{
	long days = local->tm_yday - utc->tm_yday;
	if (local->tm_year != utc->tm_year)
		days = local->tm_year > utc->tm_year ? 1 : -1;
	const long hours = days * HOURS_PER_DAY + local->tm_hour - utc->tm_hour;
	return ((hours * MINUTES_PER_HOUR) + local->tm_min - utc->tm_min) * SECONDS_PER_MINUTE + local->tm_sec -
		utc->tm_sec;
}

// localtime of now is UTC moved by the console's biases, read here from its settings, and mktime takes it back. C's
// offsets are east of UTC, the console's west.
static void check_time_zone(void)
{
	int32_t bias = 0;
	int32_t standard_bias = 0;
	int32_t daylight_bias = 0;
	const bool read = read_setting(XCONFIG_USER_TIME_ZONE_BIAS, &bias, sizeof(bias)) &&
		read_setting(XCONFIG_USER_TIME_ZONE_STD_BIAS, &standard_bias, sizeof(standard_bias)) &&
		read_setting(XCONFIG_USER_TIME_ZONE_DLT_BIAS, &daylight_bias, sizeof(daylight_bias));
	const time_t now = time(NULL);
	struct tm local;
	struct tm utc;
	localtime_r(&now, &local);
	gmtime_r(&now, &utc);
	const bool daylight = local.tm_isdst > 0;
	const long ahead = seconds_ahead(&local, &utc);
	const long expected = -(long)(bias + (daylight ? daylight_bias : standard_bias)) * SECONDS_PER_MINUTE;
	struct tm round_trip = local;
	const bool taken_back = mktime(&round_trip) == now;
	const char *const tz = getenv("TZ");
	xsdk_log_write(
		"settings: time zone %s, TZ %s; biases %d, standard %d, daylight %d minutes west; daylight saving "
		"%s; local time %02d:%02d, %ld s ahead of UTC",
		tzname[daylight], tz ? tz : "unset", (int)bias, (int)standard_bias, (int)daylight_bias, daylight ? "on" : "off",
		local.tm_hour, local.tm_min, ahead);
	check("settings: localtime is UTC moved by the console's time zone, and mktime takes it back",
		read && ahead == expected && taken_back);
}

// The language and country, 0 for neither, logged with what the console's settings hold for each.
static void check_language_and_country(void)
{
	const uint32_t language = xsdk_system_language();
	const uint32_t country = xsdk_system_country();
	uint32_t settings_language = 0;
	uint8_t settings_country = 0;
	(void)read_setting(XCONFIG_USER_LANGUAGE, &settings_language, sizeof(settings_language));
	(void)read_setting(XCONFIG_USER_COUNTRY, &settings_country, sizeof(settings_country));
	xsdk_log_write(
		"settings: language %u, country %u in XAM's locales; the console's settings hold language %u, "
		"country %u",
		(unsigned)language, (unsigned)country, (unsigned)settings_language, (unsigned)settings_country);
	check("settings: the console's language and country read", language != 0 && country != 0);
}

void test_system(void)
{
	check_time_zone();
	check_language_and_country();
}
