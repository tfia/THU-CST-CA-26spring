#!/bin/bash

set -e

make

for stride in 8 16 32 64 128; do
    echo "Probing cache size with stride ${stride}..."
    build/probe $stride > ./results/results_stride_${stride}.txt
done