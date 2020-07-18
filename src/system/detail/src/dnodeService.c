/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE

#include "os.h"

#include "dnodeSystem.h"
#include "tglobalcfg.h"
#include "tsdb.h"
#include "vnode.h"

/* Termination handler */
void signal_handler(int signum, siginfo_t *sigInfo, void *context) {
  if (signum == SIGUSR1) {
    tsCfgDynamicOptions("debugFlag 135");
    return;
  }
  if (signum == SIGUSR2) {
    tsCfgDynamicOptions("resetlog");
    return;
  }
  syslog(LOG_INFO, "Shut down signal is %d", signum);
  syslog(LOG_INFO, "Shutting down TDengine service...");
  // clean the system.
  dPrint("shut down signal is %d, sender PID:%d", signum, sigInfo->si_pid);
  dnodeCleanUpSystem();
  // close the syslog
  syslog(LOG_INFO, "Shut down TDengine service successfully");
  dPrint("TDengine is shut down!");
  closelog();
  exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
  // Set global configuration file
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-c") == 0) {
      if (i < argc - 1) {
        strcpy(configDir, argv[++i]);
      } else {
        printf("'-c' requires a parameter, default:%s\n", configDir);
        exit(EXIT_FAILURE);
      }
    } else if (strcmp(argv[i], "-V") == 0) {
      char *versionStr = tsIsCluster ? "enterprise" : "community";
      printf("%s version: %s compatible_version: %s\n", versionStr, version, compatible_version);
      printf("gitinfo: %s\n", gitinfo);
      printf("gitinfoI: %s\n", gitinfoOfInternal);
      printf("buildinfo: %s\n", buildinfo);
      return 0;
    } else if (strcmp(argv[i], "-k") == 0) {
      dnodeParseParameterK();
#ifdef TAOS_MEM_CHECK
    } else if (strcmp(argv[i], "--alloc-random-fail") == 0) {
      if ((i < argc - 1) && (argv[i+1][0] != '-')) {
        taosSetAllocMode(TAOS_ALLOC_MODE_RANDOM_FAIL, argv[++i], true);
      } else {
        taosSetAllocMode(TAOS_ALLOC_MODE_RANDOM_FAIL, NULL, true);
      }
    } else if (strcmp(argv[i], "--detect-mem-leak") == 0) {
      if ((i < argc - 1) && (argv[i+1][0] != '-')) {
        taosSetAllocMode(TAOS_ALLOC_MODE_DETECT_LEAK, argv[++i], true);
      } else {
        taosSetAllocMode(TAOS_ALLOC_MODE_DETECT_LEAK, NULL, true);
      }
#endif
    }
  }

  /* Set termination handler. */
  struct sigaction act;
  act.sa_flags = SA_SIGINFO;
  act.sa_sigaction = signal_handler;
  sigaction(SIGTERM, &act, NULL);
  sigaction(SIGHUP, &act, NULL);
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGUSR1, &act, NULL);
  sigaction(SIGUSR2, &act, NULL);
  // sigaction(SIGABRT, &act, NULL);

  // Open /var/log/syslog file to record information.
  openlog("TDengine:", LOG_PID | LOG_CONS | LOG_NDELAY, LOG_LOCAL1);
  syslog(LOG_INFO, "Starting TDengine service...");

  // Initialize the system
  if (dnodeInitSystem() < 0) {
    syslog(LOG_ERR, "Error initialize TDengine system");
    closelog();

    dnodeCleanUpSystem();
    exit(EXIT_FAILURE);
  }

  syslog(LOG_INFO, "Started TDengine service successfully.");

  while (1) {
    sleep(1000);
  }
}
