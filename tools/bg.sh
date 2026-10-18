#!/bin/bash
# bg.sh NAME cmd...  : run a command in the background, output to build/bg/NAME.out
mkdir -p "$(dirname "$0")/../build/bg"
N=$1; shift
nohup "$@" > "$(dirname "$0")/../build/bg/$N.out" 2>&1 &
echo started $N
