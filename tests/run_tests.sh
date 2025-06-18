#!/bin/sh
set -e
g++ -std=c++11 test.cpp -o test
./test

# cleanup
rm -f test sample_in.txt sample_out.txt
