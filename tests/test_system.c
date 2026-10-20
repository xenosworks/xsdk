#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tests.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/system.h"

enum {
	SECONDS_PER_MINUTE = 60,
	MINUTES_PER_HOUR = 60,
	HOURS_PER_DAY = 24,
	// Room past Win32's 172 bytes, filled with the pattern, to see how much XAM writes.
	TIME_ZONE_ROOM_BYTES = 512,
	PATTERN = 0xA5,
	NAME_BYTES = TIME_ZONE_NAME_CHARS + 1,
	DATE_BYTES = 96,
};

typedef union {
	TIME_ZONE_INFORMATION info;
	uint8_t room[TIME_ZONE_ROOM_BYTES];
} TimeZoneRoom;

_Static_assert(sizeof(TIME_ZONE_INFORMATION) == 172, "Win32's TIME_ZONE_INFORMATION is 172 bytes");

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

// A UTF-16 name as ASCII, '?' for anything else.
static void ascii_name(char name[NAME_BYTES], const uint16_t *wide)
{
	size_t length = 0;
	for (; length < TIME_ZONE_NAME_CHARS && wide[length]; length++)
		name[length] = wide[length] < 0x80 ? (char)wide[length] : '?';
	name[length] = '\0';
}

static void format_date(char text[DATE_BYTES], const SYSTEMTIME *date)
{
	(void)snprintf(text, DATE_BYTES, "%u-%u-%u, day of the week %u, %u:%02u:%02u.%03u", date->year, date->month,
		date->day, date->day_of_week, date->hour, date->minute, date->second, date->milliseconds);
}

// What GetTimeZoneInformation gave, and how many bytes of the pattern it wrote over, which Win32's layout puts at 172.
static void log_time_zone(const TimeZoneRoom *zone, uint32_t id)
{
	size_t written = sizeof(zone->room);
	while (written > 0 && zone->room[written - 1] == PATTERN)
		written--;
	const TIME_ZONE_INFORMATION *const info = &zone->info;
	char standard_name[NAME_BYTES];
	char daylight_name[NAME_BYTES];
	char standard_date[DATE_BYTES];
	char daylight_date[DATE_BYTES];
	ascii_name(standard_name, info->standard_name);
	ascii_name(daylight_name, info->daylight_name);
	format_date(standard_date, &info->standard_date);
	format_date(daylight_date, &info->daylight_date);
	xsdk_log_write(
		"settings: GetTimeZoneInformation gave %u and wrote %u bytes; bias %d; standard \"%s\", bias %d, "
		"from %s; daylight \"%s\", bias %d, from %s",
		(unsigned)id, (unsigned)written, (int)info->bias, standard_name, (int)info->standard_bias, standard_date,
		daylight_name, (int)info->daylight_bias, daylight_date);
}

// localtime of now is UTC moved by GetTimeZoneInformation's bias, with its daylight or standard bias as it says, which
// localtime agrees with, and mktime takes it back. C's offsets are east of UTC, Windows' west.
static void check_time_zone(void)
{
	static TimeZoneRoom zone;
	memset(&zone, PATTERN, sizeof(zone));
	const uint32_t id = GetTimeZoneInformation(&zone.info);
	log_time_zone(&zone, id);
	const TIME_ZONE_INFORMATION *const info = &zone.info;
	const bool daylight = id == TIME_ZONE_ID_DAYLIGHT;
	// With neither, the bias alone (wine:dlls/kernelbase/locale.c:7353-7362).
	const int32_t extra_bias = daylight ? info->daylight_bias : id == TIME_ZONE_ID_STANDARD ? info->standard_bias : 0;
	const time_t now = time(NULL);
	struct tm local;
	struct tm utc;
	localtime_r(&now, &local);
	gmtime_r(&now, &utc);
	const long ahead = seconds_ahead(&local, &utc);
	const long expected = -(long)(info->bias + extra_bias) * SECONDS_PER_MINUTE;
	struct tm round_trip = local;
	const bool taken_back = mktime(&round_trip) == now;
	const char *const tz = getenv("TZ");
	xsdk_log_write(
		"settings: time zone %s, TZ %s; daylight saving %s to newlib, %s to XAM; local time %02d:%02d, %ld s "
		"ahead of UTC",
		tzname[local.tm_isdst > 0], tz ? tz : "unset", local.tm_isdst > 0 ? "on" : "off", daylight ? "on" : "off",
		local.tm_hour, local.tm_min, ahead);
	check("settings: localtime is UTC moved by the console's time zone, and mktime takes it back",
		id != TIME_ZONE_ID_INVALID && ahead == expected && (local.tm_isdst > 0) == daylight && taken_back);
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
