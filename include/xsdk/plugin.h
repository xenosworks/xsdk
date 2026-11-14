#ifndef XSDK_PLUGIN_H
#define XSDK_PLUGIN_H

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// A plugin is a system DLL that stays loaded across titles, as DashLaunch loads them from launch.ini; it links
// libxsdk_plugin.a alone (cmake/xsdk.cmake, xsdk_add_plugin; docs/GUIDE.md, "Plugins").

// The plugin's own, as main is a title's: it runs on a system thread of its own once the kernel has loaded the plugin,
// with the plugin's handle. Returning unloads the plugin and ends the thread.
void plugin_main(HANDLE module);

// Sends xsdk_plugin_log's lines to the end of the file at path, a device path in a folder that exists, such as
// "\\Device\\Harddisk0\\Partition1\\XSDK\\plugin.log"; path is kept, not copied.
void xsdk_plugin_log_open(const char *path);
// One line, formatted by the kernel's printf, from the plugin's threads; the file is opened for each line, so lines
// keep coming across titles.
void xsdk_plugin_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif
