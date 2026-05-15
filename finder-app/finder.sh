#!/bin/sh
#
# Author: KaywebDev

if [ $# -lt 2 ]; then
    echo "Error: Two arguments required: <filesdir> <searchstr>"
    exit 1
fi

filesdir=$1
searchstr=$2

if [ ! -d "$filesdir" ]; then
    echo "Error: ${filesdir} is not a directory"
    exit 1
fi

num_files=$(find "$filesdir" -type f 2>/dev/null | wc -l)

num_matches=$(grep -R -- "$searchstr" "$filesdir" 2>/dev/null | wc -l)

echo "The number of files are ${num_files} and the number of matching lines are ${num_matches}"
exit 0