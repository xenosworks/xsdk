#ifndef XSDK_SYSTEM_H
#define XSDK_SYSTEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The console's settings. Its time zone, from XAM's GetTimeZoneInformation, is newlib's from start-up, as TZ, so
// localtime, mktime and strftime's %Z give the console's local time, daylight saving included; a title's own
// setenv("TZ", ...) and tzset() replace it.

// XGetLanguage's numbers (xenia:src/xenia/xbox.h:330-346).
typedef enum {
	XSDK_LANGUAGE_ENGLISH = 1,
	XSDK_LANGUAGE_JAPANESE = 2,
	XSDK_LANGUAGE_GERMAN = 3,
	XSDK_LANGUAGE_FRENCH = 4,
	XSDK_LANGUAGE_SPANISH = 5,
	XSDK_LANGUAGE_ITALIAN = 6,
	XSDK_LANGUAGE_KOREAN = 7,
	XSDK_LANGUAGE_TRADITIONAL_CHINESE = 8,
	XSDK_LANGUAGE_PORTUGUESE = 9,
	XSDK_LANGUAGE_SIMPLIFIED_CHINESE = 10,
	XSDK_LANGUAGE_POLISH = 11,
	XSDK_LANGUAGE_RUSSIAN = 12,
} XsdkLanguage;

// The console's language, XGetLanguage's number, as XsdkLanguage names them.
uint32_t xsdk_system_language(void);
// The console's country, XamGetLocaleEx's number for it in XAM's list of locales: 35 Britain, 36 the US
// (xenia:src/xenia/kernel/xam/xam_locale.cc:110-119).
uint32_t xsdk_system_country(void);

#ifdef __cplusplus
}
#endif

#endif
