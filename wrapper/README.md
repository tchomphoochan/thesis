You must set environmental variables `BOARD` (e.g. `vcu108`, `verilator`, `sim`, `sim_bloom`) and `PMHW_DIR` for most targets here.

Make Targets:
- `make generate` generates all files necessary to interface hardware.
Everything in `generated` folder is what you would use for a project that needs to interface with the hardware.
You could have taken this, but the point of this project is to hide Connectal entirely.
- `make all`: Build the ncessary files for a specific board. You can copy these files into your project.
- `make clean`: Clean intermediate files, except Connectal-generated ones.
- `make mrproper`: Clean everything.

`src` and `include` contains the implementation of wrapper we're actually trying to build.

`obj` contains the intermediate build files generated from the above. The files are separated according to `BOARD`.

`output` is the minimal set of files you should copy into your projects. The files are separated according to `BOARD`.
