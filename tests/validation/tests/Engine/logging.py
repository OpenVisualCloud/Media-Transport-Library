# INTEL CONFIDENTIAL
# Copyright 2024-2024 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials, and your use of them is governed
# by the express license under which they were provided to you ("License"). Unless the License provides otherwise,
# you may not use, modify, copy, publish, distribute, disclose or transmit this software or the related documents
# without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no express or implied warranties,
# other than those that are expressly stated in the License.
import datetime
import logging
import os
import pytest
import shutil
import subprocess
from typing import Dict

from .const import LOG_FOLDER
from .csv_report import csv_add_test, csv_write_report
from .stash import clear_result_log, get_result_log, clear_issue, get_issue, clear_result_note, get_result_note

phase_report_key = pytest.StashKey[Dict[str, pytest.CollectReport]]()

BANNER = 19
TESTCMD = 18
CMD = 17
STDOUT = 16
DMESG = 15
PASSED = 14
FAILED = 13
SKIPPED = 12
RESULT = 11


def log_banner(msg, *args, **kwargs):
    if logging.getLogger().isEnabledFor(BANNER):
        logging.log(BANNER, msg)


def log_testcmd(msg, *args, **kwargs):
    if logging.getLogger().isEnabledFor(TESTCMD):
        logging.log(TESTCMD, msg)


def log_cmd(msg, *args, **kwargs):
    if logging.getLogger().isEnabledFor(CMD):
        logging.log(CMD, msg)


def log_stdout(msg, *args, **kwargs):
    if logging.getLogger().isEnabledFor(STDOUT):
        logging.log(STDOUT, msg)


def log_dmesg(msg, *args, **kwargs):
    if logging.getLogger().isEnabledFor(DMESG):
        logging.log(DMESG, msg)


def log_passed(msg, *args, **kwargs):
    if logging.getLogger().isEnabledFor(PASSED):
        logging.log(PASSED, msg)


def log_failed(msg, *args, **kwargs):
    if logging.getLogger().isEnabledFor(FAILED):
        logging.log(FAILED, msg)


def log_skipped(msg, *args, **kwargs):
    if logging.getLogger().isEnabledFor(SKIPPED):
        logging.log(SKIPPED, msg)


def log_result(msg, *args, **kwargs):
    if logging.getLogger().isEnabledFor(RESULT):
        logging.log(RESULT, msg)


@pytest.hookimpl(wrapper=True, tryfirst=True)
def pytest_runtest_makereport(item, call):
    # execute all other hooks to obtain the report object
    rep = yield

    # store test results for each phase of a call, which can
    # be "setup", "call", "teardown"
    item.stash.setdefault(phase_report_key, {})[rep.when] = rep

    return rep


@pytest.fixture(scope="session", autouse=True)
def log_session():
    logging.addLevelName(BANNER, "BANNER")
    logging.banner = log_banner
    logging.Logger.banner = log_banner
    logging.addLevelName(TESTCMD, "TESTCMD")
    logging.testcmd = log_testcmd
    logging.Logger.testcmd = log_testcmd
    logging.addLevelName(CMD, "CMD")
    logging.cmd = log_cmd
    logging.Logger.cmd = log_cmd
    logging.addLevelName(STDOUT, "STDOUT")
    logging.stdout = log_stdout
    logging.Logger.stdout = log_stdout
    logging.addLevelName(DMESG, "DMESG")
    logging.dmesg = log_dmesg
    logging.Logger.dmesg = log_dmesg
    logging.addLevelName(PASSED, "PASSED")
    logging.passed = log_passed
    logging.Logger.passed = log_passed
    logging.addLevelName(FAILED, "FAILED")
    logging.failed = log_failed
    logging.Logger.failed = log_failed
    logging.addLevelName(SKIPPED, "SKIPPED")
    logging.skipped = log_skipped
    logging.Logger.skipped = log_skipped
    logging.addLevelName(RESULT, "RESULT")
    logging.result = log_result
    logging.Logger.result = log_result

    today = datetime.datetime.today()
    folder = today.strftime("%Y-%m-%dT%H:%M:%S")
    path = os.path.join(LOG_FOLDER, folder)
    path_symlink = os.path.join(LOG_FOLDER, "latest")
    try:
        os.remove(path_symlink)
    except FileNotFoundError:
        pass
    os.makedirs(path, exist_ok=True)
    os.symlink(folder, path_symlink)

    logging.banner("#" * 60)
    logging.banner("##### Tests started: " + str(datetime.datetime.today()))
    logging.banner("#" * 60)
    logging.banner("")

    yield

    shutil.copy("pytest.log", f"{LOG_FOLDER}/latest/pytest.log")
    csv_write_report(f"{LOG_FOLDER}/latest/report.csv")


@pytest.fixture(scope="function", autouse=True)
def log_case(request, caplog: pytest.LogCaptureFixture):
    case_id = request.node.nodeid
    case_folder = os.path.dirname(case_id)
    os.makedirs(os.path.join(LOG_FOLDER, "latest", case_folder), exist_ok=True)

    logfile = os.path.join(LOG_FOLDER, "latest", f"{case_id}.log")
    fh = logging.FileHandler(logfile)
    fh.setLevel(logging.DEBUG)
    format = logging.Formatter("%(asctime)s %(levelname)-8s %(message)s")
    fh.setFormatter(format)
    logger = logging.getLogger()
    logger.addHandler(fh)

    clear_result_log()
    clear_issue()

    logging.banner("#" * 60)
    logging.banner("##### Test case: " + case_id)
    logging.banner("#" * 60)
    logging.banner("")

    if os.environ.get("dmesg") == "clear":
        os.system("dmesg -C")
    else:
        dmesg_log = f"##### Test case {request.node.nodeid} #####\n"
        with open("/dev/kmsg", "w") as file:
            file.write(dmesg_log)

    yield

    logging.banner("")
    logging.banner("#" * 20 + "    DMESG OUTPUT    " + "#" * 20)
    logging.banner("")

    with subprocess.Popen(
        "exec dmesg -H", stdout=subprocess.PIPE, stderr=subprocess.STDOUT, shell=True, text=True
    ) as proc:
        dmesg = "".join(proc.stdout)

    if os.environ.get("dmesg") != "clear":
        dmesg = dmesg.split(dmesg_log)[-1]
    for line in dmesg.splitlines():
        logging.dmesg(line.rstrip())

    logging.banner("")
    logging.banner("#" * 25 + "  RESULT  " + "#" * 25)

    report = request.node.stash[phase_report_key]
    if report["setup"].failed:
        logging.failed("Setup failed")
        logging.result(f"FAILED {case_id}")
        os.chmod(logfile, 0o4755)
        result = "Fail"
    elif ("call" not in report) or report["call"].failed:
        logging.failed("Test failed")
        logging.result(f"FAIL {case_id}")
        os.chmod(logfile, 0o4755)
        result = "Fail"
    elif report["call"].passed:
        logging.passed("Test passed")
        logging.result(f"PASS {case_id}")
        os.chmod(logfile, 0o755)
        result = "Pass"
    else:
        logging.skipped("Test skipped")
        logging.result(f"SKIP {case_id}")
        result = "Skip"

    for line in get_result_log():
        logging.result(line)
    logging.result("")

    logger.removeHandler(fh)

    commands = []
    for record in caplog.get_records("call"):
        if record.levelno == TESTCMD:
            commands.append(record.message)

    csv_add_test(
        test_case=case_id, commands="\n".join(commands), result=result, issue=get_issue(), result_note=get_result_note()
    )

    clear_result_note()
