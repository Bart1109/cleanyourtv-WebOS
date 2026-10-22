#!/bin/sh
# Build the directory that gets bind-mounted over LG's Home: the launcher page plus an appinfo.json derived
# from this TV's own stock Home (nothing of LG's is shipped in the repo). Safe to re-run while mounted.
# With $L/NATIVE present (and the native launcher intact) the directory holds the native launcher instead of the page.
L=/media/developer/cleanyourtv
S=$L/home-overlay
NB=/media/developer/apps/usr/palm/applications/com.cleanyourtv.native/bin/launcher
mkdir -p $S
cp $L/app/* $S/
if [ -e $L/NATIVE ] && [ -x $NB ] && ! LD_TRACE_LOADED_OBJECTS=1 /lib/ld-linux.so.3 $NB 2>&1 | grep -q "not found"; then
  mkdir -p $S/bin; cp $NB $S/bin/launcher; chmod 755 $S/bin/launcher
  node $L/jtool.js stage "$L/backup/home-appinfo.orig.json" "$S/appinfo.json" native
else
  rm -rf $S/bin
  node $L/jtool.js stage "$L/backup/home-appinfo.orig.json" "$S/appinfo.json"
fi
