#!/usr/bin/env bash

RETURN=0
CLANG_FORMAT=$(which clang-format)
if [ $? -ne 0 ]; then
    echo "[!] clang-format not installed. Unable to check source file format policy." >&2
    exit 1
fi

FILES=`git diff --cached --name-only --diff-filter=ACMR | grep -E "\.(c|cpp|h)$"`
for FILE in $FILES; do
    $CLANG_FORMAT -i $FILE
done

