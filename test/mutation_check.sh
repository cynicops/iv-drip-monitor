#!/usr/bin/env bash
# Sanity check for the test suite itself: inject small, realistic bugs into a
# copy of the sketch and make sure at least one test fails for each of them.
# Usage: test/mutation_check.sh      (run from the repo root or test/)
set -u
cd "$(dirname "$0")"
SKETCH=../firmware/IV_Drip_Monitor/IV_Drip_Monitor.ino
mkdir -p build
survived=0
mutate() {  # $1 = sed expression, $2 = description
  sed "$1" "$SKETCH" > build/mutant.ino
  if cmp -s "$SKETCH" build/mutant.ino; then echo "  ??  $2 (pattern not found)"; survived=1; return; fi
  g++ -std=c++17 -O2 -Imocks -DSKETCH='"build/mutant.ino"' -o build/mutant test_main.cpp -pthread 2>/dev/null
  if ./build/mutant > build/mutant.log 2>&1; then
    echo "  SURVIVED  $2"; survived=1
  else
    echo "  caught    $2  ($(grep -c '^FAIL' build/mutant.log) tests failed)"
  fi
}
echo "Mutation check:"
mutate 's/if (chat != CHAT_ID) continue;/;/'                                   "Telegram accepts commands from any chat"
mutate 's/STOP_MIN_MS        = 20000;/STOP_MIN_MS        = 10000;/'            "stop timeout shortened to 10 s"
mutate 's/(now - lastTrigMs) > REFRACTORY_MS/(now - lastTrigMs) > 0/'          "refractory period removed"
mutate 's/avgIntervalMs = sum \/ filled;/avgIntervalMs = sum \/ AVG_DROPS;/'   "average divides by the wrong count"
mutate 's/if (resetIntervals) {   /if (false) {   /'                            "average not reset after a stop"
mutate 's/(now % 1000) < 200/(now % 1000) < 500/'                              "beep length changed"
mutate 's/TOLERANCE = 0.20;/TOLERANCE = 0.40;/'                                "alarm tolerance widened to 40%"
mutate 's/DEVIATION_ALARM_MS = 30000;/DEVIATION_ALARM_MS = 0;/'               "no grace period for fast/slow"
mutate 's/fabsf(dev) > DETECT_THRESHOLD \&\&/dev < -DETECT_THRESHOLD \&\&/'     "only darkening drops detected"
mutate 's/if (!isMuted() \&\& /if (/; s/alarm \&\& !isMuted() \&\&/alarm \&\&/' "mute ignored by the buzzer"
mutate 's/bagLowSent = true;/;/'                                               "bag-low message repeats"
mutate 's/digitalWrite(PIN_IR_LED, LOW);/digitalWrite(PIN_IR_LED, HIGH);/'     "LED never switched off (no ambient cancelling)"
rm -f build/mutant build/mutant.ino build/mutant.log
exit $survived
