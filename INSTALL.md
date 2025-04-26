First, make sure you generate a SSH key pair and add the public key to your Github account.

Then, make sure your `.bashrc` or `.bash_profile` (or whatever variant you use) include the contents from `dotfiles`.
Read through the files and understand what they do.

Finally, set up the machine by running `scripts/install.sh`.
Again, please read through the script first before running it.
This script was designed specifically for Ubuntu machines.
Alternatively, if you are using Arch Linux, you can install
the packages provided in <https://github.com/tchomphoochan/my-arch-pkgs>.

This prcoess installs all the necessary pre-requisites for building, synthesizing, and running Connectal-based hardware projects.
