#!/bin/bash
echo "Update in progress . . ."
sudo rm -f /usr/local/bin/quill
rm editor.cpp uninstall.sh
wget https://raw.githubusercontent.com/Olivenda/Editor/main/olid.tar -O olid.tar
tar -xvf olid.tar
# Compile the C++ editor
g++ -o quill editor.cpp
mov quill /home
cd /home
sudo mv olid /usr/local/bin/quill

# Ensure it is executable
sudo chmod +x /usr/local/bin/quill
echo "Update finished"



