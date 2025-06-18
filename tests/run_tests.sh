#!/bin/sh
set -e
if [ ! -d quill ]; then
    tar xf ../quill.tar
fi

g++ -std=c++11 test.cpp -o test
./test

# cleanup
rm -rf quill test sample_in.txt sample_out.txt
