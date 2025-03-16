# The build grid

Keeping track of `pmhw` bitstreams for multiple configurations is annoying.

This script allows you to build Puppetmaster for any given configurations based on a source repository.
It automatically caches the original repo alongside any generated bitstream, ensuring everything is up to date.
If the bitstream is already up to date with the original repo, then it does not needlessly re-synthesize the bitstream.

Usage:
- Change the global values in `build-grid.py` (as marked by `TODO`).
- Run the command.
```
./build-grid.py <action> <config>
```
- `<action> = check` checks whether the bitstream exists and whether it is up to date with the source repo.
- `<action> = build` builds a new bitstream that is up to date with source repo if needed.
- `<action> = load` flashes the bitstream.
- `<config>` should be in the form of `x,x,x,x,x,x,x,x,x` or simply `"all"`