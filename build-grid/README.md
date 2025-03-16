# The build grid

Keeping track of `pmhw` bitstreams for multiple configurations is annoying.

This script allows you to declare an original, "template" version of Puppetmaster
and a list of configurations to use. Running the script will generate all the work directories
corresponding to all these configurations. It also generates symbolic links for
the bistream files, making it easier to load different bitstreams quickly.

Usage:
- Change the global values in `build-grid.py` (as marked by `TODO`).
- Run `python3 build-grid.py`.