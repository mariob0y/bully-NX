/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "util.h"
#include "error.h"

#include "config.h"

void fatal_error(const char *fmt, ...) {
  va_list list;

  // Log to bully_log.txt & bully_crash.txt
  FILE *lf = fopen(LOG_NAME, "a");
  if (lf) {
    fprintf(lf, "\n============================================================\n");
    fprintf(lf, "=== BULLY NX FATAL ERROR ===================================\n");
    va_start(list, fmt);
    vfprintf(lf, fmt, list);
    va_end(list);
    fprintf(lf, "\n============================================================\n");
    fflush(lf);
    fclose(lf);
  }

  FILE *cf = fopen("bully_crash.txt", "a");
  if (cf) {
    fprintf(cf, "\n============================================================\n");
    fprintf(cf, "=== BULLY NX FATAL ERROR ===================================\n");
    va_start(list, fmt);
    vfprintf(cf, fmt, list);
    va_end(list);
    fprintf(cf, "\n============================================================\n");
    fflush(cf);
    fclose(cf);
  }

  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  consoleInit(NULL);

  va_start(list, fmt);
  vprintf(fmt, list);
  va_end(list);

  printf("\n\nPress A to exit.");

  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 keys = padGetButtonsDown(&pad);
    if (keys & HidNpadButton_A) break;
  }

  consoleExit(NULL);
  exit(1);
}
