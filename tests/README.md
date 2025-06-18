# Tests

This directory contains a very small regression test for `loadFile()` and
`saveFile()` using plain `assert` calls.

## Running

Execute the helper script:

```bash
./run_tests.sh
```

The script extracts `quill.tar`, compiles `test.cpp` with `g++` and runs
 the resulting binary. Temporary files are removed afterwards.
