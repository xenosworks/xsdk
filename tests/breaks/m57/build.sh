#!/bin/sh
# Run in WSL; optional arguments select names from cases.tsv.
set -eu
cd "$(dirname "$0")/../../.."
exec python3 tests/breaks/m57/build.py "$@"
