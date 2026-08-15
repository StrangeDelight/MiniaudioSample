#!/usr/bin/env sh
#
# r - run the program
#
# With no arguments it plays the mono sample (which is a C#, MIDI note 61)
# from the first MIDI keyboard. Options are added on top of that; a file name
# replaces the default sample entirely.
#   ./r                              mono sample, first keyboard
#   ./r --port 2                     mono sample, MIDI port 2
#   ./r other.raw --note 60 --rate 44100

if [ ! -x ./play ]; then
  echo "./play not found - run ./c first to compile it"
  exit 1
fi

# No file given (nothing, or the first word is an --option)? Use the default sample.
case "$1" in
  ""|--*) set -- samples/sample_mono.raw --note 61 "$@" ;;
esac

exec ./play "$@"
