/* util.c -- misc utility functions
 *
 * Copyright (C) 2026 givethesourceplox, fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "util.h"
#include "config.h"

static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_main_thread;
static int s_main_thread_set = 0;
static int s_compat_delay_ms = 2;

void debugPrintf_setMainThread(void)
{
  s_main_thread = pthread_self();
  s_main_thread_set = 1;
}

void debugPrintf_setCompatDelayMs(int ms)
{
  if (ms < 0)
    ms = 0;
  if (ms > 20)
    ms = 20;
  s_compat_delay_ms = ms;
}

#ifdef DEBUG_LOG

static int s_nxlinkSock = -1;

static void initNxLink(void)
{
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void)
{
  if (s_nxlinkSock >= 0)
  {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

void userAppInit(void)
{
  initNxLink();
}

void userAppExit(void)
{
  deinitNxLink();
}

#endif

#include <signal.h>

static void crash_signal_handler(int sig)
{
  FILE *f = fopen(LOG_NAME, "a");
  if (f)
  {
    fprintf(f, "\n\n========================================================\n");
    fprintf(f, "=== FATAL CRASH: SIGNAL %d RECEIVED ===\n", sig);
    switch (sig)
    {
    case SIGSEGV: fprintf(f, "Signal: SIGSEGV (Segmentation Fault / Invalid Memory Access)\n"); break;
    case SIGBUS:  fprintf(f, "Signal: SIGBUS (Bus Error / Alignment Fault)\n"); break;
    case SIGILL:  fprintf(f, "Signal: SIGILL (Illegal Instruction)\n"); break;
    case SIGABRT: fprintf(f, "Signal: SIGABRT (Abort / Assertion Failure)\n"); break;
    case SIGFPE:  fprintf(f, "Signal: SIGFPE (Floating Point Exception)\n"); break;
    default:      fprintf(f, "Signal: %d\n", sig); break;
    }
    u64 mem_total = 0, mem_used = 0;
    svcGetInfo(&mem_total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    fprintf(f, "Memory: %llu / %llu MB used (%.1f%%)\n",
            (unsigned long long)(mem_used >> 20),
            (unsigned long long)(mem_total >> 20),
            mem_total ? (double)mem_used / (double)mem_total * 100.0 : 0.0);
    fprintf(f, "========================================================\n\n");
    fflush(f);
    fclose(f);
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

void install_crash_handler(void)
{
  signal(SIGSEGV, crash_signal_handler);
  signal(SIGBUS,  crash_signal_handler);
  signal(SIGILL,  crash_signal_handler);
  signal(SIGFPE,  crash_signal_handler);
  signal(SIGABRT, crash_signal_handler);
}

int debugPrintf(const char *text, ...)
{
#ifdef DEBUG_LOG
  va_list list;
  const char *tag = "?";
  if (s_main_thread_set)
    tag = pthread_equal(pthread_self(), s_main_thread) ? "M" : "G";

  pthread_mutex_lock(&s_log_mutex);

#if DEBUG_FILE_LOG
  FILE *f = fopen(LOG_NAME, "a");
  if (f)
  {
    fprintf(f, "[%s] ", tag);
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fflush(f);
    fclose(f);
  }
#endif

  printf("[%s] ", tag);
  va_start(list, text);
  vprintf(text, list);
  va_end(list);

#if !DEBUG_FILE_LOG
  // DEBUG_FILE_LOG=1 keeps this mutex held much longer because every log line
  // does synchronous file I/O. That extra serialization is currently masking a
  // startup/gameplay race in the port, so emulate a small portion of that
  // delay without actually writing debug.log.
  if (s_compat_delay_ms > 0)
    svcSleepThread((int64_t)s_compat_delay_ms * 1000000LL);
#endif

  pthread_mutex_unlock(&s_log_mutex);
#endif
  return 0;
}

int ret0(void) { return 0; }

int ret1(void) { return 1; }

int retm1(void) { return -1; }
