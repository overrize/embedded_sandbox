/*
 * The one thing the board can say that settles "is this actually the
 * firmware I just built?".
 *
 * That question kept coming up and kept being answered by inference --
 * comparing file timestamps on the PC and hoping the download really
 * took. It does not always: Ozone reads the ELF once at project load and
 * a later download flashes the copy it already has, so a rebuild can be
 * invisible from the debugger side. Guessing wrong wastes a whole test
 * round on the previous image.
 *
 * This file is deliberately recompiled on EVERY build (see the FORCE
 * prerequisite in the Makefile), so __DATE__/__TIME__ are the moment the
 * firmware was actually built rather than the last time this one file
 * happened to change. The console prints it (`ver`), so the device
 * itself answers the question instead of the PC's file dates.
 */
#include "board_buildid.h"

const char mdl_build_id[] = __DATE__ " " __TIME__;
