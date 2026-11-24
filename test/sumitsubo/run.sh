#!/bin/sh
# Host tests for the study core. Needs g++ and a deck: sumitsubo deck <folder> on the desktop.
# usage: test/sumitsubo/run.sh <folder holding sumitsubo/deck>
set -e
here=$(cd "$(dirname "$0")" && pwd)
lib=$here/../../lib/Sumitsubo
work=$(mktemp -d)
cp -r "$1/sumitsubo" "$work/"
g++ -std=c++17 -O1 -Wall -Wextra -I"$lib" "$here/test_core.cpp" "$lib"/*.cpp -o "$work/test_core"
"$work/test_core" "$here/fsrs_vectors.txt" "$work"
