#!/usr/bin/env python3

"""
So... I tried having Claude implement what took me three hours to implement, and it seemed to do a pretty good job.
The code is also a lot easier to read, so here it is.

https://claude.ai/share/b088ca96-33ff-455d-94ae-3352e582aa3a
"""

import os
import sys
import shutil
import hashlib
import subprocess
from pathlib import Path
import fnmatch
import re

# Constants
SRC_DIR = "./pmhw-src"
BUILD_CACHE_DIR = "./pmhw-build-cache"
FPGA_BOARD = "vcu108"  # The FPGA board type
CONFIG_TEMPLATE = """
typedef {LogNumberRenamerThreads} LogNumberRenamerThreads;
typedef {LogNumberShards} LogNumberShards;
typedef {LogSizeShard} LogSizeShard;
typedef {LogNumberHashes} LogNumberHashes;
typedef {LogNumberComparators} LogNumberComparators;
typedef {LogNumberSchedulingRounds} LogNumberSchedulingRounds;
typedef {LogNumberPuppets} LogNumberPuppets;
typedef {NumberAddressOffsetBits} NumberAddressOffsetBits;
typedef {LogSizeRenamerBuffer} LogSizeRenamerBuffer;
"""

# Default configurations
DEFAULT_CONFIGS = {
    "all": ["2,2,6,6,1,1,3,6,7"]
}

def load_gitignore_patterns():
    """Load patterns from .gitignore file."""
    patterns = []
    gitignore_path = os.path.join(SRC_DIR, ".gitignore")
    
    if os.path.exists(gitignore_path):
        with open(gitignore_path, "r") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    patterns.append(line)
    
    # Always ignore git files
    patterns.extend([".git", ".gitignore", ".gitattributes"])
    return patterns

def should_ignore(file_path, ignore_patterns):
    """Check if a file should be ignored based on gitignore patterns."""
    rel_path = os.path.relpath(file_path, SRC_DIR)
    
    # Check if file is in .git directory
    if ".git" in rel_path.split(os.path.sep):
        return True
    
    for pattern in ignore_patterns:
        if fnmatch.fnmatch(rel_path, pattern) or fnmatch.fnmatch(os.path.basename(rel_path), pattern):
            return True
    
    return False

def get_src_hash():
    """Generate a hash of the original source repo, respecting .gitignore."""
    hasher = hashlib.sha256()
    ignore_patterns = load_gitignore_patterns()
    
    for root, dirs, files in os.walk(SRC_DIR):
        # Remove ignored directories from the walk
        dirs[:] = [d for d in dirs if not should_ignore(os.path.join(root, d), ignore_patterns)]
        
        for file in sorted(files):  # Sort to ensure consistent hash
            file_path = os.path.join(root, file)
            if os.path.isfile(file_path) and not should_ignore(file_path, ignore_patterns):
                with open(file_path, "rb") as f:
                    hasher.update(f.read())
    
    return hasher.hexdigest()

def parse_configs(config_arg):
    """Parse multiple configurations from the argument."""
    configs = []
    
    # Handle 'all' keyword
    if config_arg == "all":
        return DEFAULT_CONFIGS["all"]
    
    # Handle multiple configs separated by semicolons
    for config_str in config_arg.split(';'):
        config_str = config_str.strip()
        if config_str == "all":
            configs.extend(DEFAULT_CONFIGS["all"])
        else:
            configs.append(config_str)
    
    return configs

def parse_config(config_str):
    """Parse a single configuration string into a list of values."""
    try:
        values = config_str.split(',')
        if len(values) != 9:
            raise ValueError("Configuration must have exactly 9 values")
        return values
    except Exception as e:
        print(f"Error parsing configuration: {e}")
        sys.exit(1)

def get_identifier(config):
    """Generate an identifier string from the configuration."""
    return f"config-{'_'.join(config)}"

def create_config_file(config, build_dir):
    """Create the PmConfig.bsv file with the given configuration."""
    config_file_path = os.path.join(build_dir, "bsv", "PmConfig.bsv")
    os.makedirs(os.path.dirname(config_file_path), exist_ok=True)
    
    # Map configuration values to the template
    config_content = CONFIG_TEMPLATE.format(
        LogNumberRenamerThreads=config[0],
        LogNumberShards=config[1],
        LogSizeShard=config[2],
        LogNumberHashes=config[3],
        LogNumberComparators=config[4],
        LogNumberSchedulingRounds=config[5],
        LogNumberPuppets=config[6],
        NumberAddressOffsetBits=config[7],
        LogSizeRenamerBuffer=config[8]
    )
    
    with open(config_file_path, "w") as f:
        f.write(config_content)

def build(config_str):
    """Build the Puppetmaster with the given configuration."""
    config = parse_config(config_str)
    identifier = get_identifier(config)
    build_dir = f"./pmhw-build-{identifier}"
    
    # Create build directory and copy source
    if os.path.exists(build_dir):
        shutil.rmtree(build_dir)
    shutil.copytree(SRC_DIR, build_dir)
    
    # Create config file
    create_config_file(config, build_dir)
    
    # Clean the board-specific directory first to ensure a clean build
    board_dir = os.path.join(build_dir, FPGA_BOARD)
    if os.path.exists(board_dir):
        print(f"Cleaning {FPGA_BOARD} directory...")
        shutil.rmtree(board_dir)
    
    # Run make with the specific board target
    print(f"Building configuration: {config_str} for {FPGA_BOARD}")
    try:
        subprocess.run(["make", f"build.{FPGA_BOARD}"], cwd=build_dir, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Build failed: {e}")
        sys.exit(1)
    
    # Copy result to cache
    os.makedirs(BUILD_CACHE_DIR, exist_ok=True)
    bit_file = os.path.join(build_dir, FPGA_BOARD, "hw", "mkTop.bit")
    if not os.path.exists(bit_file):
        print(f"Build completed but {bit_file} was not found")
        sys.exit(1)
    
    # Save the build output
    shutil.copy(bit_file, os.path.join(BUILD_CACHE_DIR, f"{identifier}.bit"))
    
    # Save the source hash
    src_hash = get_src_hash()
    with open(os.path.join(BUILD_CACHE_DIR, f"{identifier}.hash"), "w") as f:
        f.write(src_hash)
    
    print(f"Build successful. Output saved to {BUILD_CACHE_DIR}/{identifier}.bit")
    
    # Cleanup
    shutil.rmtree(build_dir)

def check(config_str):
    """Check if the build for the given configuration exists and is up to date."""
    config = parse_config(config_str)
    identifier = get_identifier(config)
    
    bit_file = os.path.join(BUILD_CACHE_DIR, f"{identifier}.bit")
    hash_file = os.path.join(BUILD_CACHE_DIR, f"{identifier}.hash")
    
    # Check if bit file exists
    if not os.path.exists(bit_file):
        print(f"Warning: {bit_file} does not exist. Build required.")
        return False
    
    # Check if hash file exists
    if not os.path.exists(hash_file):
        print(f"Warning: {hash_file} does not exist. Hash verification not possible.")
        return False
    
    # Check if hash matches
    with open(hash_file, "r") as f:
        stored_hash = f.read().strip()
    
    current_hash = get_src_hash()
    if stored_hash != current_hash:
        print(f"Warning: Source code has changed since the last build.")
        print(f"Stored hash: {stored_hash}")
        print(f"Current hash: {current_hash}")
        return False
    
    print(f"Build for configuration {config_str} exists and is up to date.")
    return True

def load(config_str):
    """Load the bitfile for the given configuration onto the FPGA."""
    if config_str == "all" or ";" in config_str:
        print("Error: 'load' command only accepts a single configuration, not 'all' or multiple configs.")
        sys.exit(1)
    
    config = parse_config(config_str)
    identifier = get_identifier(config)
    bit_file = os.path.join(BUILD_CACHE_DIR, f"{identifier}.bit")
    
    # Check if the build exists and is up to date
    is_current = check(config_str)
    
    if not os.path.exists(bit_file):
        print(f"Error: Bitfile for configuration {config_str} not found.")
        sys.exit(1)
    
    if not is_current:
        response = input("The bitfile is out of date. Do you want to proceed with loading anyway? (y/n): ")
        if response.lower() not in ["y", "yes"]:
            print("Loading cancelled.")
            sys.exit(0)
    
    # Load the bitfile onto the FPGA
    print(f"Loading configuration {config_str} onto FPGA...")
    try:
        subprocess.run(["fpgajtag", bit_file], check=True)
        print("FPGA programmed successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Error loading bitfile: {e}")
        sys.exit(1)
    except FileNotFoundError:
        print("Error: fpgajtag command not found.")
        sys.exit(1)

def main():
    if len(sys.argv) < 3:
        print("Usage: ./build-grid.py [build|check|load] <config>")
        print("       <config> can be a single configuration (x,x,x,x,x,x,x,x,x),")
        print("       multiple configurations separated by semicolons,")
        print("       or 'all' for default configurations.")
        sys.exit(1)
    
    command = sys.argv[1]
    config_arg = sys.argv[2]
    
    # Ensure the source directory exists
    if not os.path.isdir(SRC_DIR):
        print(f"Error: Source directory {SRC_DIR} does not exist.")
        sys.exit(1)
    
    # Process commands
    if command == "build":
        configs = parse_configs(config_arg)
        for config in configs:
            build(config)
    elif command == "check":
        configs = parse_configs(config_arg)
        for config in configs:
            check(config)
    elif command == "load":
        load(config_arg)
    else:
        print(f"Unknown command: {command}")
        print("Usage: ./build-grid.py [build|check|load] <config>")
        sys.exit(1)

if __name__ == "__main__":
    main()