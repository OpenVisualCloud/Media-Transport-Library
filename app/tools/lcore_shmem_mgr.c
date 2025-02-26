/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <errno.h>
#include <getopt.h>
#include <mtl/mtl_lcore_shm_api.h>
#include <stdlib.h>

#include "log.h"

enum lsm_args_cmd {
  LSM_ARG_UNKNOWN = 0,
  LSM_ARG_HELP = 0x100, /* start from end of ascii */
  LSM_ARG_INFO,
  LSM_ARG_CLEAN_PID_AUTO_CHECK,
  LSM_ARG_CLEAN_LCORE,
  LSM_ARG_MAX,
};

/*
struct option {
   const char *name;
   int has_arg;
   int *flag;
   int val;
};
*/
static struct option lsm_args_options[] = {
    {"help", no_argument, 0, LSM_ARG_HELP},
    {"info", no_argument, 0, LSM_ARG_INFO},
    {"clean_pid_auto_check", no_argument, 0, LSM_ARG_CLEAN_PID_AUTO_CHECK},
    {"clean_lcore", required_argument, 0, LSM_ARG_CLEAN_LCORE},
    {0, 0, 0, 0},
};

static void lsm_print_help() {
  printf("\n");
  printf("##### Usage: #####\n\n");

  printf("Params:\n");
  printf(" --help: Print the help information\n");
  printf(" --info: Print lcore shared manager detail info\n");
  printf(" --clean_pid_auto_check: Clean the dead entries if PID is not active\n");
  printf(" --clean_lcore <lcore id>: Clean the entry by lcore ID\n");

  printf("\n");
}

int main(int argc, char** argv) {
  int cmd = -1, opt_idx = 0;
  int ret;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", lsm_args_options, &opt_idx);
    if (cmd == -1) break;

    switch (cmd) {
      case LSM_ARG_INFO:
        mtl_lcore_shm_print();
        break;
      case LSM_ARG_CLEAN_PID_AUTO_CHECK:
        ret = mtl_lcore_shm_clean(MTL_LCORE_CLEAN_PID_AUTO_CHECK, NULL, 0);
        if (ret > 0)
          info("Total %d dead lcores detected and deleted\n", ret);
        else if (ret == 0)
          info("No dead lcores detected\n");
        else
          err("Fail %d to clean shm by auto PID check\n", ret);
        break;
      case LSM_ARG_CLEAN_LCORE: {
        int lcore = atoi(optarg);
        if (lcore < 0) {
          err("lcore %d is not valid\n", lcore);
          return -EIO;
        }
        struct mtl_lcore_clean_pid_info pid;
        pid.lcore = lcore;
        ret = mtl_lcore_shm_clean(MTL_LCORE_CLEAN_LCORE, &pid, sizeof(pid));
        if (ret >= 0)
          info("Succ to delete lcore %d\n", lcore);
        else
          err("Fail %d to delete lcore %d\n", ret, lcore);
        break;
      }
      case LSM_ARG_HELP:
      default:
        lsm_print_help();
        return -1;
    }
  }

  return 0;
}