#include "xsdk/system.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "system_private.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

// The console's settings: its time zone, made newlib's at start-up, its language and its country.

enum {
	// newlib's shortest zone name, and room for its longest (newlib:newlib/libc/time/tzset_r.c:12-13).
	ZONE_NAME_MIN = 3,
	ZONE_NAME_BYTES = 11,
	TZ_BYTES = 96,
	MINUTES_PER_HOUR = 60,
	// POSIX's TZ takes offsets up to 24 hours.
	MAX_OFFSET_MINUTES = 24 * MINUTES_PER_HOUR,
	// What XamGetLocale asks of XamGetLocaleEx (xenia:src/xenia/kernel/xam/xam_locale.cc:225).
	MAX_COUNTRY = 111,
	MAX_LOCALE = 43,
};

typedef struct {
	int32_t bias;
	int32_t standard_bias;
	int32_t daylight_bias;
	XCONFIG_TIMEZONE_DATE standard_date;
	XCONFIG_TIMEZONE_DATE daylight_date;
	char standard_name[XCONFIG_TIME_ZONE_NAME_BYTES + 1];
	char daylight_name[XCONFIG_TIME_ZONE_NAME_BYTES + 1];
} TimeZone;

static bool read_setting(uint16_t setting, void *value, uint16_t value_bytes)
{
	uint16_t required_bytes;
	const NTSTATUS status = ExGetXConfigSetting(XCONFIG_USER_CATEGORY, setting, value, value_bytes, &required_bytes);
	if (!NT_SUCCESS(status))
		xsdk_log_write("time zone: ExGetXConfigSetting(%u) failed: 0x%08X", setting, (unsigned)status);
	return NT_SUCCESS(status);
}

static bool read_time_zone(TimeZone *zone)
{
	*zone = (TimeZone){ 0 };
	return read_setting(XCONFIG_USER_TIME_ZONE_BIAS, &zone->bias, sizeof(zone->bias)) &&
		read_setting(XCONFIG_USER_TIME_ZONE_STD_NAME, zone->standard_name, XCONFIG_TIME_ZONE_NAME_BYTES) &&
		read_setting(XCONFIG_USER_TIME_ZONE_DLT_NAME, zone->daylight_name, XCONFIG_TIME_ZONE_NAME_BYTES) &&
		read_setting(XCONFIG_USER_TIME_ZONE_STD_DATE, &zone->standard_date, sizeof(zone->standard_date)) &&
		read_setting(XCONFIG_USER_TIME_ZONE_DLT_DATE, &zone->daylight_date, sizeof(zone->daylight_date)) &&
		read_setting(XCONFIG_USER_TIME_ZONE_STD_BIAS, &zone->standard_bias, sizeof(zone->standard_bias)) &&
		read_setting(XCONFIG_USER_TIME_ZONE_DLT_BIAS, &zone->daylight_bias, sizeof(zone->daylight_bias));
}

// Whether a date fits a POSIX rule, Mmonth.week.day/hour, which counts as Windows' time zones do.
static bool date_fits(const XCONFIG_TIMEZONE_DATE *date)
{
	return date->month >= 1 && date->month <= 12 && date->day >= 1 && date->day <= 5 && date->day_of_week <= 6 &&
		date->hour <= 23;
}

// Writes a zone as TZ has it, <name>offset: the console's name, letters and digits only, or else the offset east of
// UTC, as tzdata names a zone without one. The offset is west of UTC in both TZ and the console's settings.
static int append_zone(char *tz, size_t tz_bytes, const char *console_name, int32_t minutes_west)
{
	const int32_t magnitude = minutes_west < 0 ? -minutes_west : minutes_west;
	const int hours = (int)(magnitude / MINUTES_PER_HOUR);
	const int minutes = (int)(magnitude % MINUTES_PER_HOUR);
	char name[ZONE_NAME_BYTES];
	size_t length = 0;
	for (const char *c = console_name; *c; c++) {
		if (isalnum((unsigned char)*c))
			name[length++] = *c;
	}
	name[length] = '\0';
	if (length < ZONE_NAME_MIN)
		(void)snprintf(name, sizeof(name), "%c%02d%02d", minutes_west > 0 ? '-' : '+', hours, minutes);
	return snprintf(tz, tz_bytes, "<%s>%s%d:%02d", name, minutes_west < 0 ? "-" : "", hours, minutes);
}

void xsdk_system_set_time_zone(void)
{
	TimeZone zone;
	if (!read_time_zone(&zone))
		return;
	const int32_t standard_west = zone.bias + zone.standard_bias;
	const int32_t daylight_west = zone.bias + zone.daylight_bias;
	if (abs(standard_west) > MAX_OFFSET_MINUTES || abs(daylight_west) > MAX_OFFSET_MINUTES) {
		xsdk_log_write("time zone: offsets of %d and %d minutes; UTC kept", (int)standard_west, (int)daylight_west);
		return;
	}
	char tz[TZ_BYTES];
	const int length = append_zone(tz, sizeof(tz), zone.standard_name, standard_west);
	const XCONFIG_TIMEZONE_DATE *const start = &zone.daylight_date;
	const XCONFIG_TIMEZONE_DATE *const end = &zone.standard_date;
	if (date_fits(start) && date_fits(end)) {
		const int daylight_length = append_zone(tz + length, sizeof(tz) - length, zone.daylight_name, daylight_west);
		(void)snprintf(tz + length + daylight_length, sizeof(tz) - length - daylight_length,
			",M%u.%u.%u/%u,M%u.%u.%u/%u", start->month, start->day, start->day_of_week, start->hour, end->month,
			end->day, end->day_of_week, end->hour);
	} else if (start->month || end->month) {
		xsdk_log_write("time zone: daylight saving %u.%u.%u/%u to %u.%u.%u/%u isn't a POSIX rule; none kept",
			start->month, start->day, start->day_of_week, start->hour, end->month, end->day, end->day_of_week,
			end->hour);
	}
	if (setenv("TZ", tz, 1) != 0) {
		xsdk_log_write("time zone: setenv(TZ, %s) failed", tz);
		return;
	}
	tzset();
}

uint32_t xsdk_system_language(void)
{
	return XGetLanguage();
}

uint32_t xsdk_system_country(void)
{
	return XamGetLocaleEx(MAX_COUNTRY, MAX_LOCALE);
}
