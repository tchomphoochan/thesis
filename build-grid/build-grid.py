#!/usr/bin/env python3

import logging
import sys
import shutil
import os
import subprocess
import argparse
from typing import Literal

# ###
# TODO: Set the following variables before running the script
# ###

# Points to the source directory of pmhw to be used for the build grid.
# The directory is expected to be clean: no build artifacts, etc.
ORIG_DIR = "pmhw-src"

# Config values in the same order as `PMCONFIG_TMPL` below.
CONFIG_VALUES = [(2, 2, 6, 6, 1, 1, 3, 6, 7)]

# Board to generate bitstreams for
BOARD = "vcu108"

# ###
# Do not change the following variables.
# ###

WORKDIR = "pmhw-workdir"
CACHE_DIR = "pmhw-bitstream-cache"
CONFIG_PATH = os.path.join("bsv", "PmConfig.bsv")
BITFILE_PATH = os.path.join(BOARD, "hw", "mkTop.bit")
HASH_PATH = "tar.md5"
CONFIG_TMPL = """typedef {} LogNumberRenamerThreads;
typedef {} LogNumberShards;
typedef {} LogSizeShard;
typedef {} LogNumberHashes;
typedef {} LogNumberComparators;
typedef {} LogNumberSchedulingRounds;
typedef {} LogNumberPuppets;
typedef {} NumberAddressOffsetBits;
typedef {} LogSizeRenamerBuffer;
"""
CONFIG_LEN = CONFIG_TMPL.count("{}")
CONFIG_NAME_TMPL = "-".join(["{}"] * CONFIG_LEN)  # {}-{}-{}-...-{}  CONFIG_LEN times

# ###
# Script starts here.
# ###


def main(
    logger: logging.Logger,
    action: Literal["check", "build", "load"],
    configs: list[tuple[int]],
):
    # Ensures all necessary directories exist
    if not os.path.exists(ORIG_DIR):
        logger.critical(f"{ORIG_DIR = } must exist.")
        sys.exit(os.EX_NOINPUT)

    if not os.path.isdir(ORIG_DIR):
        logger.critical(f"{ORIG_DIR = } is not a directory but it should be.")
        sys.exit(os.EX_NOINPUT)

    os.makedirs(CACHE_DIR, exist_ok=True)
    os.makedirs(WORKDIR, exist_ok=True)

    # Heuristics to check that the repository is clean
    p = os.path.join(ORIG_DIR, CONFIG_PATH)
    if os.path.exists(p):
        logger.critical(
            f"{p} exists, suggesting this is not a clean directory. Please clean."
        )
        sys.exit(os.EX_NOINPUT)

    p = os.path.join(ORIG_DIR, BOARD)
    if os.path.exists(p):
        logger.critical(
            f"{p} exists, suggesting this is not a clean directory. Please clean."
        )
        sys.exit(os.EX_NOINPUT)

    # Gather the hash of the source directory
    hash_p = subprocess.run(
        ["sh", "-c", "tar --exclude='.git' -c %s | md5sum | awk '{ print $1 }'" % ORIG_DIR],
        capture_output=True,
    )
    hash = hash_p.stdout.decode("utf-8").strip()
    logger.info(f"Note, {ORIG_DIR} has hash = {hash}")

    # Do each config
    for config in configs:
        handle_config(action, config, hash, logger=logger)


def handle_config(
    action: Literal["check", "build", "load"],
    config: tuple[int],
    hash: str,
    *,
    logger: logging.Logger,
):
    print(f"Doing {action} on {config}...")
    bitfile_path, up_to_date = check_config(config, hash, logger=logger)

    if action == "check":
        return

    if action == "load":
        print("Loading the configuration onto FPGA...")
        subprocess.run(["fpgajtag", bitfile_path])
        return

    if action == "build":
        print("Building the configuration...")
        build_config(config, hash, logger=logger)

    raise RuntimeError("Should not reach here")


def check_config(
    config: tuple[int], expected_hash: str, *, logger: logging.Logger
) -> tuple[str | None, bool]:
    """
    Check whether a bitstream exists for the given configuration, and if so, is it up-to-date.
    Returns (path to bitfile or none, up-to-date).
    """

    config_name = CONFIG_NAME_TMPL.format(*config)

    bitfile_path = os.path.join(CACHE_DIR, config_name + ".bit")
    if not os.path.exists(bitfile_path):
        print(f"Bitstream file does not exist: {bitfile_path}")
        return (None, False)

    hash_path = os.path.join(CACHE_DIR, config_name + ".md5")
    if not os.path.exists(hash_path):
        print(f"Corresponding hash file does not exist: {hash_path}")
        return (bitfile_path, False)

    with open(hash_path, "r") as hash_file:
        actual_hash = hash_file.read().strip()
    if actual_hash != expected_hash:
        print(
            f"Bitstream file exists but hash does not match current source repo. Might be out of date."
        )
        print(f"Bitstream file: {bitfile_path}")
        print(f"Actual hash value: {actual_hash}")
        print(f"Expected hash value: {expected_hash}")
        return (bitfile_path, False)

    print(f"Bitstream file exists and the hash matches!")
    print(f"Bitstream file: {bitfile_path}")
    print(f"Hash value: {expected_hash}")
    return (bitfile_path, True)


def build_config(config: tuple[int], hash: str, *, logger: logging.Logger):
    config_name = CONFIG_NAME_TMPL.format(*config)

    # Create a work directory corresponding to that config
    # Example: ./pmhw-workdir/<config>/
    folder_path = os.path.join(WORKDIR, config_name)
    logger.info(f"Creating {folder_path}")
    subprocess.run(["rsync", "-h", "-r", ORIG_DIR + "/", folder_path + "/"])

    # Add the config file
    # Corresponds to: cp ./pmhw-src/DefaultPmConfig.bsv ./pmhw-workdir/<config>/bsv/PmConfig.bsv
    config_path = os.path.join(folder_path, CONFIG_PATH)
    with open(config_path, "w") as config_file:
        content = CONFIG_TMPL.format(*config)
        config_file.write(content)

    # Add a hash file
    # Corresponds to: touch ./pmhw-workdir/<config>/tar.md5
    built_hash_path = os.path.join(folder_path, HASH_PATH)
    with open(built_hash_path, "w") as hash_file:
        hash_file.write(hash)

    # Make sure to remove any lingering builds
    # Corresponds to: rm -fR ./pmhw-workdir/<config>/vcu108
    board_path = os.path.join(WORKDIR, config_name, BOARD)
    if os.path.exists(board_path):
        shutil.rmtree(board_path)

    # All good now. Run the build.
    # Corresponds to: make -C ./pmhw-workdir/<config> build.vcu108
    subprocess.run(["make", f"build.{BOARD}"], cwd=folder_path)

    # Extract the build.
    built_bitfile_path = os.path.join(WORKDIR, config_name, BITFILE_PATH)
    if not os.path.exists(built_bitfile_path):
        logger.error(f"Failed to build: {built_bitfile_path}")
        return

    # Corresponds to: cp ./pmhw-workdir/<config>/vcu108/hw/mkTop.bit ./pmhw-bitstream-cache/<config>.bit
    #               : cp ./pmhw-workdir/<config>/tar.md5            ./pmhw-bitstream-cache/<config>.md5
    bitfile_path = os.path.join(CACHE_DIR, config_name + ".bit")
    hash_path = os.path.join(CACHE_DIR, config_name + ".md5")
    shutil.copy(built_bitfile_path, bitfile_path)
    shutil.copy(built_hash_path, hash_path)


def build_args_parser(*, logger: logging.Logger) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Command line utility to manage multiple Puppetmaster builds."
    )
    parser.add_argument("action", choices=["check", "build", "load"])
    parser.add_argument("config")
    return parser


def parse_configs(
    action: str, text: str, *, logger: logging.Logger
) -> list[tuple[int]]:
    if text == "all":
        if action == "load":
            logger.critical("Cannot load all configurations at the same time")
            sys.exit(os.EX_USAGE)
        else:
            return CONFIG_VALUES

    entries = text.split(";")
    configs = [tuple(int(val) for val in entry.split(",")) for entry in entries]
    if len(configs) > 1 and action == "load":
        logger.critical("Cannot load more than one configurations at a time")
        sys.exit(os.EX_USAGE)

    for config in configs:
        assert len(config) == 9

    return configs


if __name__ == "__main__":
    formatter = logging.Formatter("[{asctime} - {levelname}] {message}", style="{")
    console_handler = logging.StreamHandler()
    console_handler.setFormatter(formatter)
    console_handler.setLevel(logging.INFO)

    logger = logging.getLogger(__name__)
    logger.setLevel(logging.INFO)
    logger.addHandler(console_handler)

    parser = build_args_parser(logger=logger)
    args = parser.parse_args()
    main(logger, args.action, parse_configs(args.action, args.config, logger=logger))
