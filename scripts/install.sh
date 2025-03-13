N=$(nproc)

# Make sure all necessary environmental variables are set
#
if ! [[ -z "$BSPATH" ]]; then
  echo "Make sure BSPATH is set."
  exit 1
else

if ! [[ -z "$FPGAMAKE" ]]; then
  echo "Make sure FPGAMAKE is set."
  exit 1
else

if ! [[ -z "$CONNECTALDIR" ]]; then
  echo "Make sure CONNECTALDIR is set."
  exit 1
else

# This script will try to install most things onto local root. There are some things that are automatically installed
# to system root, such as `sudo make install` as required by Connectal and whatnot,
# but in my experience it's best to keep those to the minimum. (Connectal can be quite weird to install.)
LOCAL_ROOT="$HOME/.local"
mkdir -p $LOCAL_ROOT
SRC_DIR="$LOCAL_ROOT/src"
mkdir -p $SRC_DIR
BIN_DIR="$LOCAL_ROOT/bin"
mkdir -p $BIN_DIR
SHARE_DIR="$LOCAL_ROOT/src"
mkdir -p $SHARE_DIR

# Install useful tools
sudo apt install -y -qq tmux htop build-essential curl libffi-dev libffi8 libgmp-dev libgmp10 libncurses-dev pkg-config iverilog flex bison tcl-dev autoconf gperf verilator

################################################
# Haskell installation
################################################

# Install Haskell (locally)
if ! [ -d $HOME/.ghcup ]; then
  echo Installing Haskell...
  curl --proto '=https' --tlsv1.2 -sSf https://get-ghcup.haskell.org | sh
  echo Restart the shell then re-run this script.
  echo Restarting helps ensure that all Haskell-related environmental variables have been properly set.
else
  echo Haskell already installed. Skipping installing Haskell.
fi	

################################################
# Bluespec installation
################################################

# Bluespec dependencies
echo Installing Bluespec dependencies
cabal update
cabal v1-install regex-compat syb old-time split

# Clone Bluespec source
if ! [ -d $SRC_DIR/bsc ]; then
  echo Cloning Bluespec...
  git clone --recursive https://github.com/B-Lang-org/bsc.git $SRC_DIR/bsc
else
  echo Skipping cloning Bluespec.
fi
# Build Bluespec and move to correct place
if ! [ -d $BSPATH ]; then
  echo Installing Bluespec...
  mkdir -p $BSPATH
  make PREFIX=$BSPATH -j$N install-src -C $SRC_DIR/bsc
else
  echo Skip installing Bluespec...
fi

# Clone bsc-contrib
if ! [ -d $SRC_DIR/bsc-contrib ]; then
  echo Cloning bsc-contrib...
  git clone --recursive https://github.com/B-Lang-org/bsc-contrib.git $SRC_DIR/bsc-contrib
else
  echo Skipping cloning bsc-contrib.
fi
# Install bsc-contrib
echo Installing bsc-contrib...
make PREFIX=$BSPATH -C $SRC_DIR/bsc-contrib install

################################################
# Connectal installation
################################################

# Clone fpgamake
# fpgamake's `make install` target insists on installing to /usr/local/... which I'm not a fan of.
# Luckily, it's only just one Python script anyway so we can directly make a symlink.
if ! [ -d $FPGAMAKE ]; then
  echo Cloning fpgamake...
  git clone --recursive https://github.com/cambridgehackers/fpgamake.git $FPGAMAKE
  echo Creating symlink...
  ln -s $FPGAMAKE/fpgamake $BIN_DIR/fpgamake
else
  echo Skipping cloning fpgamake.
fi

# Clone fpgajtag
if ! [ -d $FPGAJTAG_SRC ]; then
  echo Cloning fpgajtag...
  git clone --recursive https://github.com/cambridgehackers/fpgajtag.git $SRC_DIR/fpgajtag
else
  echo Skipping cloning fpgajtag.
fi
echo Installing fpgajtag...
make PREFIX=$LOCAL_ROOT BINDIR=$BIN_DIR -C $SRC_DIR/fpgajtag/src install

# Clone Connectal
# A lot of Connectal code assumes you have the entire source available,
# so make install is really for the sake of installing the drivers.
if ! [ -d $CONNECTALDIR ]; then
  echo Cloning connectal...
  git clone --recursive https://github.com/cambridgehackers/connectal.git $SRC_DIR/connectal
else
  echo Skipping cloning connectal.
fi
# Install connectal drivers
echo Installing connectal...
sudo make -j$N install -C $CONNECTALDIR
