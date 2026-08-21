#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Split a binary and join it back up again. Anything that survives the
# round trip byte for byte means the interleave and the geometry agree
# in both directions, which is the only thing that stops a burnt set of
# ROMs coming out scrambled.

set -u

ROMJAK=${1:?usage: roundtrip.sh <path to romjak>}

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT INT TERM

pass=0
fail=0

ok()    { pass=$((pass + 1)); echo "ok   - $1"; }
notok() { fail=$((fail + 1)); echo "FAIL - $1"; }

# The names split writes, in the order join wants them back
romnames() { # <base> <numroms> <rombanks>
	base=$1 nr=$2 nb=$3
	perbank=$((nr / nb))
	if [ "$nb" -eq 1 ]; then
		j=0
		while [ $j -lt "$perbank" ]; do
			printf '%s.%d ' "$base" "$j"
			j=$((j + 1))
		done
	else
		i=0
		while [ $i -lt "$nb" ]; do
			j=0
			while [ $j -lt "$perbank" ]; do
				printf '%s.%d.%d ' "$base" "$i" "$j"
				j=$((j + 1))
			done
			i=$((i + 1))
		done
	fi
}

roundtrip() { # <input> <numroms> <rombanks> <romwidth> <romsize> [extra split args...]
	src=$1 nr=$2 nb=$3 rw=$4 rs=$5
	shift 5

	desc="roundtrip $(basename "$src") numroms=$nr rombanks=$nb romwidth=$rw romsize=$rs $*"
	base=$tmp/rt
	rm -f "$tmp"/rt.* "$tmp/back.bin"

	if ! "$ROMJAK" split --numroms="$nr" --rombanks="$nb" --romwidth="$rw" \
			--romsize="$rs" "$@" "$src" "$base" >/dev/null 2>"$tmp/err"; then
		notok "$desc (split failed: $(cat "$tmp/err"))"
		return
	fi

	insz=$(wc -c < "$src")
	# shellcheck disable=SC2046 # we want the name list word split
	if ! "$ROMJAK" join --rombanks="$nb" --romwidth="$rw" --trim="$insz" \
			-o "$tmp/back.bin" $(romnames "$base" "$nr" "$nb") \
			>/dev/null 2>"$tmp/err"; then
		notok "$desc (join failed: $(cat "$tmp/err"))"
		return
	fi

	if cmp -s "$src" "$tmp/back.bin"; then
		ok "$desc"
	else
		notok "$desc (joined output differs from the input)"
	fi
}

fails() { # <description> <args...>
	desc=$1
	shift
	if "$@" >/dev/null 2>&1; then
		notok "$desc (should have been rejected but succeeded)"
	else
		ok "$desc"
	fi
}

dd if=/dev/urandom of="$tmp/in4k.bin" bs=1024 count=4 2>/dev/null
# Not a multiple of the ROM count, so the tail of the last stride is padded
dd if=/dev/urandom of="$tmp/odd.bin" bs=1 count=700 2>/dev/null

echo "# round trips"
roundtrip "$tmp/in4k.bin"  2 1  8 2048
roundtrip "$tmp/in4k.bin"  4 1  8 1024
roundtrip "$tmp/in4k.bin"  8 1  8  512
roundtrip "$tmp/in4k.bin" 16 1  8  256
roundtrip "$tmp/in4k.bin"  4 2  8 1024
roundtrip "$tmp/in4k.bin"  8 2  8  512
roundtrip "$tmp/in4k.bin" 16 4  8  256
roundtrip "$tmp/in4k.bin"  2 1 16 2048
roundtrip "$tmp/in4k.bin"  4 2 16 1024
roundtrip "$tmp/in4k.bin"  2 1 32 2048
roundtrip "$tmp/in4k.bin"  4 4 32 1024

echo "# round trips where the input has to be padded out"
roundtrip "$tmp/odd.bin"   4 1  8 1024
roundtrip "$tmp/odd.bin"   4 2 16 1024
roundtrip "$tmp/odd.bin"   4 2  8 1024 --paduptosize=1024
roundtrip "$tmp/odd.bin"   4 2  8 1024 --paduptosize=2048
roundtrip "$tmp/odd.bin"   4 2  8 1024 --paduptosize=1024 --pad=0

reversed_rt() { # <numroms> <rombanks> <romwidth> <romsize>
	nr=$1 nb=$2 rw=$3 rs=$4

	desc="reversed roundtrip numroms=$nr rombanks=$nb romwidth=$rw"
	rm -f "$tmp"/rv.* "$tmp/rvback.bin"

	"$ROMJAK" split --numroms="$nr" --rombanks="$nb" --romwidth="$rw" \
			--romsize="$rs" --reverse \
			"$tmp/in4k.bin" "$tmp/rv" >/dev/null 2>&1

	insz=$(wc -c < "$tmp/in4k.bin")
	# shellcheck disable=SC2046 # we want the name list word split
	"$ROMJAK" join --rombanks="$nb" --romwidth="$rw" --reverse \
			--trim="$insz" -o "$tmp/rvback.bin" \
			$(romnames "$tmp/rv" "$nr" "$nb") >/dev/null 2>&1

	if cmp -s "$tmp/in4k.bin" "$tmp/rvback.bin"; then
		ok "$desc"
	else
		notok "$desc (joined output differs from the input)"
	fi
}

echo "# round trips with the ROMs reversed"
reversed_rt  2 1  8 2048
reversed_rt  4 1  8 1024
reversed_rt  8 1  8  512
reversed_rt  4 2  8 1024
reversed_rt 16 4  8  256
reversed_rt  4 1 16 1024
reversed_rt  2 1 32 2048

echo "# reversing actually moves the data"
rm -f "$tmp"/d.* "$tmp"/s.*
"$ROMJAK" split --numroms=4 --romsize=1024 "$tmp/in4k.bin" "$tmp/d" >/dev/null 2>&1
"$ROMJAK" split --numroms=4 --romsize=1024 --reverse "$tmp/in4k.bin" "$tmp/s" >/dev/null 2>&1
swapped=yes
for n in 0 1 2 3; do
	cmp -s "$tmp/s.$n" "$tmp/d.$((3 - n))" || swapped=no
done
[ "$swapped" = yes ] && ok "--reverse turns the images end for end" \
		|| notok "--reverse turns the images end for end"

# Joining without --reverse has to produce something different, or the flag
# is being quietly ignored and a bad burn looks fine
# shellcheck disable=SC2046
"$ROMJAK" join -o "$tmp/mismatch.bin" $(romnames "$tmp/s" 4 1) >/dev/null 2>&1
if cmp -s "$tmp/in4k.bin" "$tmp/mismatch.bin"; then
	notok "joining a reversed set without --reverse is caught"
else
	ok "joining a reversed set without --reverse is caught"
fi

echo "# padding and repeats land where they should"
rm -f "$tmp"/rep.*
"$ROMJAK" split --numroms=4 --rombanks=2 --romsize=1024 --paduptosize=1024 \
		"$tmp/odd.bin" "$tmp/rep" >/dev/null 2>&1
"$ROMJAK" join --rombanks=2 -o "$tmp/rep.bin" \
		"$tmp/rep.0.0" "$tmp/rep.0.1" "$tmp/rep.1.0" "$tmp/rep.1.1" >/dev/null 2>&1

if [ "$(wc -c < "$tmp/rep.bin")" = 4096 ]; then
	ok "4 copies of a 1024 byte slot fill the 4096 byte total"
else
	notok "4 copies of a 1024 byte slot fill the 4096 byte total"
fi

# Every 1024 byte copy must be identical
dd if="$tmp/rep.bin" of="$tmp/c0" bs=1024 count=1 skip=0 2>/dev/null
allsame=yes
for n in 1 2 3; do
	dd if="$tmp/rep.bin" of="$tmp/cn" bs=1024 count=1 skip=$n 2>/dev/null
	cmp -s "$tmp/c0" "$tmp/cn" || allsame=no
done
[ "$allsame" = yes ] && ok "every repeat is identical" \
		|| notok "every repeat is identical"

# The first 700 bytes are the input, the remaining 324 are 0xff
dd if="$tmp/c0" of="$tmp/c0data" bs=1 count=700 2>/dev/null
cmp -s "$tmp/odd.bin" "$tmp/c0data" && ok "a repeat starts with the input" \
		|| notok "a repeat starts with the input"

padvals=$(dd if="$tmp/c0" bs=1 skip=700 2>/dev/null | od -An -tx1 -v | tr -s ' \n' ' ' | tr ' ' '\n' | sort -u | grep -v '^$')
[ "$padvals" = "ff" ] && ok "the rest of a repeat is 0xff pad" \
		|| notok "the rest of a repeat is 0xff pad (saw: $padvals)"

echo "# a too-big input gets truncated to the slot, not overrun"
rm -f "$tmp"/tr.*
"$ROMJAK" split --numroms=2 --romsize=1024 --paduptosize=1024 \
		"$tmp/in4k.bin" "$tmp/tr" >/dev/null 2>&1
if [ "$(wc -c < "$tmp/tr.0")" = 1024 ] && [ "$(wc -c < "$tmp/tr.1")" = 1024 ]; then
	ok "images are romsize even when the input is bigger than the total"
else
	notok "images are romsize even when the input is bigger than the total"
fi

echo "# bad geometry is refused"
fails "too many ROMs" \
	"$ROMJAK" split --numroms=32 --romsize=128 "$tmp/in4k.bin" "$tmp/bad"
fails "too many banks" \
	"$ROMJAK" split --numroms=8 --rombanks=8 --romsize=512 "$tmp/in4k.bin" "$tmp/bad"
fails "ROMs not a multiple of banks" \
	"$ROMJAK" split --numroms=6 --rombanks=4 --romsize=512 "$tmp/in4k.bin" "$tmp/bad"
fails "ROM width not a multiple of 8" \
	"$ROMJAK" split --numroms=2 --romwidth=12 --romsize=2048 "$tmp/in4k.bin" "$tmp/bad"
fails "paduptosize does not divide the total" \
	"$ROMJAK" split --numroms=2 --romsize=2048 --paduptosize=1500 "$tmp/in4k.bin" "$tmp/bad"
fails "pad byte out of range" \
	"$ROMJAK" split --numroms=2 --romsize=2048 --pad=256 "$tmp/in4k.bin" "$tmp/bad"
fails "empty input" \
	sh -c ": > '$tmp/empty.bin'; '$ROMJAK' split --numroms=2 --romsize=16 '$tmp/empty.bin' '$tmp/bad'"
fails "join with mismatched ROM sizes" \
	"$ROMJAK" join -o "$tmp/bad.bin" "$tmp/in4k.bin" "$tmp/odd.bin"
fails "join with a missing ROM" \
	"$ROMJAK" join -o "$tmp/bad.bin" "$tmp/in4k.bin" "$tmp/not-here.bin"
fails "join trim bigger than the joined size" \
	"$ROMJAK" join --trim=99999 -o "$tmp/bad.bin" "$tmp/in4k.bin"

echo "# --dry-run writes nothing"
rm -f "$tmp"/dry.*
"$ROMJAK" split -n --numroms=2 --romsize=2048 "$tmp/in4k.bin" "$tmp/dry" >/dev/null 2>&1
[ -e "$tmp/dry.0" ] && notok "split --dry-run leaves no images" \
		|| ok "split --dry-run leaves no images"

"$ROMJAK" join -n -o "$tmp/dry.bin" "$tmp/in4k.bin" >/dev/null 2>&1
[ -e "$tmp/dry.bin" ] && notok "join --dry-run leaves no output" \
		|| ok "join --dry-run leaves no output"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
