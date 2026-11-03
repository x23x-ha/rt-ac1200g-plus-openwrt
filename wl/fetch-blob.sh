#!/bin/sh
# Where the two pieces of Broadcom code the wl image needs come from. Neither
# is in this repository: both are published by TP-Link, and you download them
# yourself. wl/build-wl.sh checks both hashes.
#
# Adapted from scripts/fetch-blob.sh of the Archer C1200 v2 port
# (https://github.com/Vygis1/archer-c1200-v2-openwrt, GPL-2.0), the same SoC
# family; this router's wl support is built on that work.
cat <<'EOF'
1. TP-Link GPL source for the Archer C1200 v2 (884 MB)
   https://www.tp-link.com/support/gpl-code/  -> search "Archer C1200"
   file   Archer_C1200v2_GPL.tar.gz   884,555,895 bytes
   sha256 01114eb1b888fa71115ecdca23d9fa74eaa325adcde6282afe6cc7af2e3e4a3b

   Used from it: the driver object wl_apsta.o (unstripped, with DWARF: that
   is what makes the 2.6.36 -> 6.12 shim possible) and the open shared code
   around it. Leave it packed; build-wl.sh unpacks the few paths it needs
   inside Docker (the tree has file names that differ only in case).

2. Archer C1200 v2 stock firmware, EU 211219 (only for the userspace tools
   wl, nas, eapd, nvram and their uClibc)
   https://www.tp-link.com/support/download/archer-c1200/v2/#Firmware
   file   Archer_C1200v2_EU_211219_v1.zip   15,332,662 bytes
   sha256 b3f6dbbb810f051fa75855dd925c0eb45225efa1ebdb9d9f743c266b2a1a0093

Then:
   GPL_TAR=/path/Archer_C1200v2_GPL.tar.gz \
   STOCK_ZIP=/path/Archer_C1200v2_EU_211219_v1.zip  wl/build-wl.sh
EOF
