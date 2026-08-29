#!/bin/sh
# profile.sh -- sample where wayvnc/neatvnc actually spend CPU, under load.
#
#   ./bench/profile.sh [wayvnc|sway] [seconds]
#
# Needs doas; DTrace needs root.
#
# What works on NetBSD/Ironlake, and what does not:
#
#   tprof          NO. "cpu not supported" -- Arrandale/Westmere PMCs are
#                  absent from tprof's x86 backend, so no hardware-counter
#                  profiling and no cache/branch statistics.
#   DTrace         YES. dtrace + dtrace_fbt + dtrace_profile load fine,
#                  ~56k probes, and ustack() resolves userland symbols out
#                  of the shared libraries. This is the usable profiler.
#   gprof          available as a fallback: rebuild with -pg. Deterministic
#                  rather than sampled, and it perturbs what it measures.
#   lockstat       available, useful if contention is ever suspected.
#
# IMPORTANT: DTrace only samples a process while it is ON CPU. Counts here
# are shares of that process's CPU time, NOT of wall time. wayvnc is close
# to idle in this workload -- roughly 1% of wall clock -- so a function
# dominating this profile can still be irrelevant to how the session feels.
# Use bench/vncbench.py to decide whether something matters; use this to
# find out where the time inside it goes.

set -e
TARGET=${1:-wayvnc}
SECS=${2:-8}
export PATH=$PATH:/usr/sbin:/sbin:/usr/pkg/bin:/usr/X11R7/bin

command -v dtrace >/dev/null || { echo "dtrace not found"; exit 1; }
for m in dtrace dtrace_fbt dtrace_profile; do
    modstat 2>/dev/null | grep -q "^$m " || doas modload "$m" 2>/dev/null || true
done

BENCH=${VNCBENCH:-/tmp/vncbench.py}
echo "-> generating load (scrolling terminal + an RFB client)"
( /usr/pkg/bin/python3.13 "$BENCH" --target wayvnc --workload scroll \
    --secs $((SECS + 8)) --repeat 1 > /tmp/profile-load.out 2>&1 ) &
LOAD=$!
sleep 5

echo "-> sampling $TARGET at 997Hz for ${SECS}s"
doas dtrace -x ustackframes=10 -n \
  "profile-997 /execname == \"$TARGET\"/ { @[ustack()] = count(); }
   tick-${SECS}s { exit(0); }" 2>&1 | tail -40

wait $LOAD 2>/dev/null || true
echo
echo "-> the load that produced it:"
tail -3 /tmp/profile-load.out
