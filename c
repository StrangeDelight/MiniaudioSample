#!/usr/bin/env sh

Pdir=$PWD
cd src
echo "Compiling program"
g++ -std=c++17 -O2 main.cpp -o ../play -lpthread -ldl -lm
strip ../play
cd $Pdir
echo
echo "Use ./play samples/sample_mono.raw"
echo "Use ./play samples/sample_mono.raw" 24000
echo "Use ./play samples/sample_stereo.raw 48000 2"
