#!/usr/bin/env python3

import libtmux
import logging
import sys
import shutil
import os
import tempfile
import subprocess

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
HASH_PATH = "dir.hash"
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
BUILD_NAME_TMPL = "-".join(["{}"] * CONFIG_LEN)  # {}-{}-{}-...-{}  CONFIG_LEN times
TMUX_SESSION_NAME = "pmhw-build-grid"

# ###
# Script starts here.
# ###


def main(logger: logging.Logger):
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
        ["sh", "-c", "tar c %s | md5sum | awk '{ print $1 }'" % ORIG_DIR],
        capture_output=True,
    )
    hash = hash_p.stdout.decode("utf-8").strip()
    logger.info(f"Note, {ORIG_DIR} has hash = {hash}")

    # For each config...
    for config in CONFIG_VALUES:
        # Create a work directory corresponding to that config
        folder_name = BUILD_NAME_TMPL.format(*config)
        folder_path = os.path.join(WORKDIR, folder_name)
        logger.info(f"Creating {folder_path}")
        subprocess.run(["rsync", "-h", "-r", ORIG_DIR + "/", folder_path + "/"])

        # Add the config file
        config_path = os.path.join(folder_path, CONFIG_PATH)
        with open(config_path, "w") as config_file:
            content = CONFIG_TMPL.format(*config)
            config_file.write(content)

        # Add a hash file
        hash_path = os.path.join(folder_path, HASH_PATH)
        with open(hash_path, "w") as hash_file:
            hash_file.write(hash)

        # Create a symlink
        bit_path = os.path.join("..", folder_path, BITFILE_PATH)
        symlink_path = os.path.join(CACHE_DIR, folder_name + ".bit")
        if os.path.islink(symlink_path):
            logger.info(f"Removing old symlink {symlink_path}")
            os.remove(symlink_path)
        logger.info(f"Creating symlink {bit_path} -> {symlink_path}")
        os.symlink(bit_path, symlink_path)

        hash_path = os.path.join("..", folder_path, HASH_PATH)
        symlink_path = os.path.join(CACHE_DIR, folder_name + ".md5")
        if os.path.islink(symlink_path):
            logger.info(f"Removing old symlink {symlink_path}")
            os.remove(symlink_path)
        logger.info(f"Creating symlink {hash_path} -> {symlink_path}")
        os.symlink(hash_path, symlink_path)


if __name__ == "__main__":
    formatter = logging.Formatter("[{asctime} - {levelname}] {message}", style="{")
    console_handler = logging.StreamHandler()
    console_handler.setFormatter(formatter)
    console_handler.setLevel(logging.INFO)

    logger = logging.getLogger(__name__)
    logger.setLevel(logging.INFO)
    logger.addHandler(console_handler)

    main(logger)
