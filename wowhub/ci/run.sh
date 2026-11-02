#!/bin/bash
# The WoW Hub check (CI): every test healer heals in its own kind of fight, a damage dealer still sims, the
# same seed gives the same result, and no healer got much slower.
#   wowhub/ci/run.sh <path to simc>      (ITER sets iterations, default 100; SKIP_SPEED=1 skips the speed check)
# The test healers in wowhub/ci/ are made up: a common talent build per spec and the same plain stats for
# everyone (no real character's gear). Speed is measured against the Arcane Mage sim on the same machine,
# so a slower or faster runner doesn't matter; speed.txt holds each sim's expected share. A run fails on an
# error, a number outside its range, a changed result for the same seed, or a sim 30% slower than expected.
set -u
SIMC=$(realpath "${1:?path to simc}"); cd "$(dirname "$0")"; OUT=$(mktemp -d); fail=0
RAID="role=heal healing=19 heal_damage_profile=raid max_time=300 vary_combat_length=0"
DUNGEON="role=heal healing=4 heal_damage_profile=dungeon max_time=300 vary_combat_length=0"
MAGE=../../profiles/MID2/MID2_Mage_Arcane.simc

sim() {  # sim <out name> <simc args...>: prints the result and the seconds taken
  local name=$1; shift
  local t0=$(date +%s.%N)
  "$SIMC" "$@" json2="$OUT/$name.json" > "$OUT/$name.txt" 2>&1
  local code=$?
  if (( code != 0 )); then
    # what went wrong: a checker's report from its first line, or the end of the output
    echo "    exit code $code$( (( code > 128 )) && echo " (signal $((code - 128)))" )" >&2
    local first=$(grep -nE 'runtime error|ERROR: AddressSanitizer|^Error' "$OUT/$name.txt" | head -1 | cut -d: -f1)
    if [[ -n "$first" ]]; then sed -n "${first},$((first + 25))p" "$OUT/$name.txt" >&2; else tail -8 "$OUT/$name.txt" >&2; fi
    return 1
  fi
  python3 - "$OUT/$name.json" "$t0" <<'PY'
import json, sys, time
p = json.load(open(sys.argv[1]))["sim"]["players"][0]["collected_data"]
heal = p.get("hps", {}).get("mean", 0) + p.get("aps", {}).get("mean", 0)
print(f"{heal if heal > 0 else p['dps']['mean']:.6f} {'HPS' if heal > 0 else 'DPS'} {time.time() - float(sys.argv[2]):.2f}")
PY
}

declare -A secs
check() {  # check <label> <key> <min> <max> <simc args...>
  local label=$1 key=$2 min=$3 max=$4; shift 4
  local r; if ! r=$(sim "$key" "$@" iterations=${ITER:-100} threads=1 target_error=0); then
    echo "FAIL  $label: the sim stopped with an error"; fail=1; return
  fi
  read -r v kind s <<< "$r"; secs[$key]=$s
  if python3 -c "import sys; sys.exit(0 if $min <= $v <= $max else 1)"; then st="ok   "; else st="FAIL "; fail=1; fi
  printf '%s %-40s %12.0f %s  (expected %s to %s)\n' "$st" "$label" "$v" "$kind" "$min" "$max"
}

echo "== Each healer in its own fight, and a damage dealer"
check "Arcane Mage (damage)" mage 50000 2000000 $MAGE
for f in *_Raid.simc; do n=${f%.simc}; check "${n//_/ } (raid fight)" "$n" 100000 2000000 "$f" $RAID; done
for f in *_Mythic.simc; do n=${f%.simc}; check "${n//_/ } (dungeon fight)" "$n" 60000 2000000 "$f" $DUNGEON; done

echo "== Same seed, same result"
for pair in "Holy_Paladin_Raid.simc:$RAID" "Mistweaver_Monk_Mythic.simc:$DUNGEON"; do
  f=${pair%%:*}; fight=${pair#*:}
  a=$(sim same1 "$f" $fight iterations=50 threads=1 seed=4242 deterministic=1 target_error=0 | cut -d' ' -f1)
  b=$(sim same2 "$f" $fight iterations=50 threads=1 seed=4242 deterministic=1 target_error=0 | cut -d' ' -f1)
  if [[ -n "$a" && "$a" == "$b" ]]; then echo "ok    ${f%.simc}: $a twice"; else echo "FAIL  ${f%.simc}: $a, then $b"; fail=1; fi
done

echo "== Speed (each sim's time / the Arcane Mage's time)"
base=${secs[mage]:-0}
[[ -n "${SKIP_SPEED:-}" ]] && { echo "skipped (SKIP_SPEED: a checking build is slow on purpose)"; secs=([mage]=0); }
for key in $(printf '%s\n' "${!secs[@]}" | sort); do
  [[ $key == mage ]] && continue
  ratio=$(python3 -c "print(f'{${secs[$key]} / $base:.2f}' if $base > 0 else 'nan')")
  want=$(awk -v k="$key" '$1 == k { print $2 }' speed.txt 2>/dev/null)
  if [[ -z "$want" ]]; then echo "new   $key $ratio (no expected value in speed.txt yet)"
  elif python3 -c "import sys; sys.exit(0 if $ratio <= $want * 1.3 else 1)"; then echo "ok    $key $ratio (expected about $want)"
  else echo "FAIL  $key $ratio, more than 30% slower than $want"; fail=1; fi
done

rm -rf "$OUT"
[[ $fail == 0 ]] && echo "All checks passed." || { echo "Some checks failed."; exit 1; }
