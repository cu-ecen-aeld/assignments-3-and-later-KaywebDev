#!/bin/sh
#
# Author: KaywebDev

if [ $# -lt 2 ]; then
    echo "Error: Two arguments required: <writefile> <writestr>"
    exit 1
fi

writefile=$1
writestr=$2

dirpath=$(dirname "$writefile")
if [ ! -d "$dirpath" ]; then
    mkdir -p "$dirpath" 2>/dev/null
    if [ $? -ne 0 ]; then
        echo "Error: Could not create directory path ${dirpath}"
        exit 1
    fi
fi

echo "$writestr" > "$writefile" 2>/dev/null
if [ $? -ne 0 ]; then
    echo "Error: Could not write to file ${writefile}"
    exit 1
fi
exit 0