#!/bin/bash
# 20-launchd.sh ROOT : daemon edits (see edit-launchd.py + lists/). DAEMON_SET=bp (default): exactly the owner build's job set;
# DAEMON_SET=min: hardware + measured crash loops only (the real iOS 27, everything else on); DISABLE_HOGS=1 adds the CPU-hog list to min.
# Jobs the set would remove can be left on (all off by default, so the default job set is unchanged; none has run on a phone yet):
#   CC_CONTROLS=1    com.apple.siriactionsd: Control Centre's controls (Calculator, Timer, Camera, Code Scanner) ask it for their
#                    intent's parameters and die without it (docs/ios27/CONTROL-CENTRE.md)
#   CRASH_REPORTS=1  com.apple.ReportCrash and com.apple.osanalytics.osanalyticshelper: iOS's own crash reports in
#                    /var/mobile/Library/Logs/CrashReporter (both start only when something crashes)
#   KEEP_JOBS="LABEL ..."  any other jobs, by exact Label or job key
# Declarative: running it again without them takes them off again (that is the undo).
set -euo pipefail
R=$1; H=$(cd "$(dirname "$0")" && pwd)
KEEP=${KEEP_JOBS:-}
[[ ${CC_CONTROLS:-0} != 1 ]] || KEEP+=" com.apple.siriactionsd"
[[ ${CRASH_REPORTS:-0} != 1 ]] || KEEP+=" com.apple.ReportCrash com.apple.osanalytics.osanalyticshelper"
KEEP=$(echo $KEEP | tr ' ' ',')
/usr/bin/python3 "$H/edit-launchd.py" "$R" "$H/../lists" --set "${DAEMON_SET:-bp}" ${DISABLE_HOGS:+--hogs} ${KEEP:+--keep "$KEEP"}
