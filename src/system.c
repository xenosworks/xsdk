#include "xsdk/system.h"

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
	// Room past Win32's 172 bytes, in case XAM's structure is longer, which the console has yet to rule out.
	TIME_ZONE_ROOM_BYTES = 512,
	// What XamGetLocale asks of XamGetLocaleEx (xenia:src/xenia/kernel/xam/xam_locale.cc:225).
	MAX_COUNTRY = 111,
	MAX_LOCALE = 43,
};

typedef union {
	TIME_ZONE_INFORMATION info;
	uint8_t room[TIME_ZONE_ROOM_BYTES];
} TimeZoneRoom;

// Whether a date makes a POSIX rule, Mmonth.week.day/time: one that recurs, which counts as POSIX's rules do.
static bool date_fits(const SYSTEMTIME *date)
{
	return date->year == 0 && date->month >= 1 && date->month <= 12 && date->day >= 1 && date->day <= 5 &&
		date->day_of_week <= 6 && date->hour <= 23 && date->minute <= 59 && date->second <= 59;
}

// Writes a zone as TZ has it, <name>offset: the name's capitals and digits, as "Eastern Standard Time" gives EST, or
// else the offset east of UTC, as tzdata names a zone without one. The offset is west of UTC in TZ, as in Windows'.
static int append_zone(char *tz, size_t tz_bytes, const uint16_t *console_name, int32_t minutes_west)
{
	const int32_t magnitude = minutes_west < 0 ? -minutes_west : minutes_west;
	const int hours = (int)(magnitude / MINUTES_PER_HOUR);
	const int minutes = (int)(magnitude % MINUTES_PER_HOUR);
	char name[ZONE_NAME_BYTES];
	size_t length = 0;
	for (size_t i = 0; i < TIME_ZONE_NAME_CHARS && console_name[i] && length < sizeof(name) - 1; i++) {
		const uint16_t c = console_name[i];
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
			name[length++] = (char)c;
	}
	name[length] = '\0';
	if (length < ZONE_NAME_MIN)
		(void)snprintf(name, sizeof(name), "%c%02d%02d", minutes_west > 0 ? '-' : '+', hours, minutes);
	return snprintf(tz, tz_bytes, "<%s>%s%d:%02d", name, minutes_west < 0 ? "-" : "", hours, minutes);
}

static int append_rule(char *tz, size_t tz_bytes, const SYSTEMTIME *date)
{
	return snprintf(tz, tz_bytes, ",M%u.%u.%u/%u:%02u:%02u", date->month, date->day, date->day_of_week, date->hour,
		date->minute, date->second);
}

void xsdk_system_set_time_zone(void)
{
	static TimeZoneRoom zone;
	const uint32_t id = GetTimeZoneInformation(&zone.info);
	const TIME_ZONE_INFORMATION *const info = &zone.info;
	if (id == TIME_ZONE_ID_INVALID) {
		xsdk_log_write("time zone: GetTimeZoneInformation failed; UTC kept");
		return;
	}
	// With no daylight saving, local time is UTC less the bias alone (wine:dlls/kernelbase/locale.c:7353-7362).
	const int32_t standard_west = info->bias + (info->daylight_date.month ? info->standard_bias : 0);
	const int32_t daylight_west = info->bias + info->daylight_bias;
	if (abs(standard_west) > MAX_OFFSET_MINUTES || abs(daylight_west) > MAX_OFFSET_MINUTES) {
		xsdk_log_write("time zone: offsets of %d and %d minutes; UTC kept", (int)standard_west, (int)daylight_west);
		return;
	}
	// At most 70 bytes: two zones of 18 and two rules of 17.
	char tz[TZ_BYTES];
	int length = append_zone(tz, sizeof(tz), info->standard_name, standard_west);
	const SYSTEMTIME *const start = &info->daylight_date;
	const SYSTEMTIME *const end = &info->standard_date;
	if (date_fits(start) && date_fits(end)) {
		length += append_zone(tz + length, sizeof(tz) - length, info->daylight_name, daylight_west);
		length += append_rule(tz + length, sizeof(tz) - length, start);
		(void)append_rule(tz + length, sizeof(tz) - length, end);
	} else if (start->month) {
		xsdk_log_write(
			"time zone: daylight saving from %u-%u-%u, day %u, %u:%02u, to %u-%u-%u, day %u, %u:%02u makes "
			"no POSIX rule; standard time kept",
			start->year, start->month, start->day, start->day_of_week, start->hour, start->minute, end->year,
			end->month, end->day, end->day_of_week, end->hour, end->minute);
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
