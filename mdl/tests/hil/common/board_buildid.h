#ifndef MDL_BOARD_BUILDID_H
#define MDL_BOARD_BUILDID_H

/*
 * Compile timestamp of THIS firmware image, "Sep  7 2026 02:02:10" style
 * (the __DATE__ " " __TIME__ format, unchanged).
 *
 * Read it off the device with the console's `ver` command. If it does not
 * match what build.ps1 printed, the download did not take -- which is a
 * real failure mode with Ozone, since it caches the ELF from project load
 * and a later download flashes that cached copy rather than re-reading
 * the file.
 *
 * board_buildid.c is rebuilt on every make invocation, so this is the
 * build time and not the time that file last changed.
 */
extern const char mdl_build_id[];

#endif /* MDL_BOARD_BUILDID_H */
