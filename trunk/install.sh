#!/bin/bash

if grep -q Fedora /etc/issue; then
# fedora system.. proceed
    exec utils/installer.pl
else
	echo ""
	echo "[1;31m[5mYou are not on a Fedora core system!![0m"
	echo ""
	echo "Ergo, you need to do a manual install."
	echo ""
    echo "[1;45m** Please read the INSTALL document for manual installation instructions **[0m"
    echo ""
fi
