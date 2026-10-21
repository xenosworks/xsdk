#ifndef XSDK_EXIT_PRIVATE_H
#define XSDK_EXIT_PRIVATE_H

// Ends the title as exit(EXIT_SUCCESS) does, then starts the .xex at xam_path, a path XAM takes, in place of the
// dashboard.
_Noreturn void xsdk_exit_launching(const char *xam_path);

#endif
