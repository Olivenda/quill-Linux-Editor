#!/bin/bash

# Remove the editor binary from /usr/local/bin
sudo rm -f /usr/local/bin/quill
rm editor.cpp
rm update.sh
cd ..
echo "Uninstallation complete! The editor has been removed."
sleep 2
rm -rf olid.cfg



