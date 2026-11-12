#!/usr/bin/env bash
set -e
BUILD_NUM_FILE="build_number.txt"
if [[ -f "$BUILD_NUM_FILE" ]]; then num=$(cat "$BUILD_NUM_FILE"); else num=0; fi
num=$((num + 1))
echo "$num" > "$BUILD_NUM_FILE"
git_hash=$(git describe --always --dirty=-dirty 2>/dev/null || echo unknown)
timestamp=$(date '+%Y-%m-%d %H:%M:%S')
cat > version_info.h <<EOF2
#pragma once
#define BUILD_NUMBER $num
#define BUILD_TIMESTAMP "$timestamp"
#define GIT_HASH "$git_hash"
EOF2
