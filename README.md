# ELAN1206 Linux TouchPad fix
C program to correct behavior of touchpad on linux
## Problem
On the ASUS UX564EH_Q528EH and potentially other laptops, the ELAN1206 touchpad is recognized and somewhat responice, but unusable. This started at kernel 6.9.
## Solution
Compile this code and activate it with a systemd script. There is some more info [here](https://bbs.archlinux.org/viewtopic.php?id=303199).
