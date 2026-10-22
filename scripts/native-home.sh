#!/bin/sh
# Make the native launcher LG's Home (on) or go back to the web launcher (off). Restarts the TV's app manager.
export PATH=/usr/bin:/usr/sbin:/bin:/sbin:$PATH
L=/media/developer/cleanyourtv
case "$1" in
  on) touch $L/NATIVE ;;
  off) rm -f $L/NATIVE ;;
  *) echo "usage: native-home.sh on|off"; exit 2 ;;
esac
sh $L/stage-home.sh
if [ "$1" = on ] && [ ! -e $L/home-overlay/bin/launcher ]; then rm -f $L/NATIVE; echo "the native launcher is missing or broken: staying on the web launcher"; exit 1; fi
H=/usr/palm/applications/com.webos.app.home
grep -q " $H " /proc/mounts || { echo "the launcher is not Home right now; it takes effect at the next start"; exit 0; }
(sleep 3; pkill -9 -f /usr/palm/applications/airplay/LunaExecutable) </dev/null >/dev/null 2>&1 &
systemctl restart sam
for i in $(seq 1 60); do systemctl is-active sam >/dev/null && break; sleep 2; done
sleep 8
pkill -f "[a]pp-id=com.webos.app.home"; pkill -f "[a]pplications/com.webos.app.home/bin/launcher"; sleep 2
luna-send -n 1 -w 10000 -f luna://com.webos.applicationManager/launch '{"id":"com.webos.app.home"}' </dev/null >/dev/null
