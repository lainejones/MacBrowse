#!/bin/bash
# synctest.sh - host test of CREATE / SYNCIN / SYNCOUT.  Run from the project directory after "make host".
# The "Mac side" is played by MacBrowse's own PUT / DELETE / MKDIR on the image.
set -u
MB=$PWD/out/macbrowse-host
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
D=$T/Shared; I=$T/shared.hfv
fail=0
ok()   { echo "ok   $1"; }
bad()  { echo "FAIL $1"; fail=1; }
have() { [ -e "$D/$1" ] && ok "$2" || bad "$2 ($1 missing)"; }
gone() { [ ! -e "$D/$1" ] && ok "$2" || bad "$2 ($1 still there)"; }
same() { cmp -s "$1" "$2" && ok "$3" || bad "$3"; }
check() { "$MB" "$I" CHECK | grep -q "Volume OK" && ok "volume consistent ($1)" || { bad "volume check ($1)"; "$MB" "$I" CHECK VERBOSE | tail -5; }; }

mkdir -p "$D/Docs/Deep"
echo "hello from the amiga" > "$D/readme.txt"
head -c 300000 /dev/urandom > "$D/Docs/big.bin"
echo "nested" > "$D/Docs/Deep/note.txt"
echo "icon" > "$D/readme.txt.info"
printf '(This file must be converted with BinHex 4.0)\n:junk:\n' > "$D/looks.hqx"

echo "== 1 first SYNCIN makes the image"
"$MB" "$I" SYNCIN "$D" NAME Amiga QUIET || bad "syncin rc"
[ -f "$I" ] && ok "image created" || bad "image created"
check 1
L=$("$MB" "$I" LS)
echo "$L" | grep -q "Amiga:readme.txt" && ok "file on the Mac" || bad "file on the Mac"
echo "$L" | grep -q "Amiga:Docs:Deep:note.txt" && ok "nested file on the Mac" || bad "nested file on the Mac"
echo "$L" | grep -q "info" && bad "icon left out" || ok "icon left out"
"$MB" "$I" GET "looks.hqx" TO "$T/x.hqx" QUIET; same "$D/looks.hqx" "$T/x.hqx" "a .hqx goes over undecoded"
"$MB" "$I" GET "Docs:big.bin" TO "$T/big.out" QUIET; same "$D/Docs/big.bin" "$T/big.out" "300 KB file identical on the Mac"

echo "== 2 nothing changed: SYNCIN copies nothing"
O=$("$MB" "$I" SYNCIN "$D")
echo "$O" | grep -q "0 to the Mac, 0 from the Mac, 0 deleted" && ok "idle sync" || { bad "idle sync"; echo "$O"; }

echo "== 3 the Mac adds, changes and deletes"
echo "written on the mac" > "$T/macnew.txt"
"$MB" "$I" PUT "$T/macnew.txt" TO "Docs" QUIET
"$MB" "$I" MKDIR "FromMac" QUIET
"$MB" "$I" PUT "$T/macnew.txt" TO "FromMac" AS "inside" QUIET
sleep 1; echo "changed on the mac, longer" > "$T/readme.txt"
"$MB" "$I" PUT "$T/readme.txt" REPLACE QUIET
"$MB" "$I" DELETE "Docs:Deep:note.txt" QUIET
"$MB" "$I" SYNCOUT "$D" QUIET || bad "syncout rc"
have "Docs/macnew.txt" "new Mac file arrives"
have "FromMac/inside" "new Mac folder arrives"
same "$D/readme.txt" "$T/readme.txt" "changed Mac file arrives"
gone "Docs/Deep/note.txt" "file deleted on the Mac is deleted here"
have "readme.txt.info" "icon untouched"
check 3

echo "== 4 the Amiga adds, changes and deletes"
sleep 1
echo "new on the amiga" > "$D/amiganew.txt"
echo "amiga changed this" > "$D/Docs/macnew.txt"
rm "$D/FromMac/inside"
"$MB" "$I" SYNCIN "$D" QUIET || bad "syncin rc"
L=$("$MB" "$I" LS)
echo "$L" | grep -q "Amiga:amiganew.txt" && ok "new Amiga file on the Mac" || bad "new Amiga file on the Mac"
echo "$L" | grep -q "FromMac:inside" && bad "deleted Amiga file gone on the Mac" || ok "deleted Amiga file gone on the Mac"
"$MB" "$I" GET "Docs:macnew.txt" TO "$T/m2" QUIET; same "$D/Docs/macnew.txt" "$T/m2" "changed Amiga file on the Mac"
check 4

echo "== 5 both sides change the same file"
sleep 1
echo "amiga version" > "$D/amiganew.txt"
echo "mac version of it" > "$T/amiganew.txt"
"$MB" "$I" PUT "$T/amiganew.txt" REPLACE QUIET
"$MB" "$I" SYNCOUT "$D" QUIET
have "amiganew.txt (Mac)" "the Mac's version is kept beside"
grep -q "amiga version" "$D/amiganew.txt" && ok "the Amiga's version is kept" || bad "the Amiga's version is kept"

echo "== 6 deleted on the Mac but changed here: it stays"
sleep 1
echo "edited after the last sync" >> "$D/Docs/big.bin"
"$MB" "$I" DELETE "Docs:big.bin" QUIET
"$MB" "$I" SYNCOUT "$D" QUIET
have "Docs/big.bin" "a changed file is not deleted"
"$MB" "$I" SYNCIN "$D" QUIET
"$MB" "$I" LS | grep -q "Docs:big.bin" && ok "and goes back to the Mac" || bad "and goes back to the Mac"
check 6

echo "== 7 the drawer outgrows the image: a new, larger one"
head -c 60000000 /dev/urandom > "$D/huge.bin"
O=$("$MB" "$I" SYNCIN "$D")
echo "$O" | grep -q "New Mac disk" && ok "image made anew" || { bad "image made anew"; echo "$O" | tail -3; }
"$MB" "$I" GET "huge.bin" TO "$T/huge.out" QUIET; same "$D/huge.bin" "$T/huge.out" "60 MB file identical"
"$MB" "$I" LS | grep -q "Docs:macnew.txt" && ok "everything else is there again" || bad "everything else is there again"
check 7

echo "== 8 a Mac file with a resource fork comes back with a sidecar and round-trips"
printf 'data fork' > "$T/withrsrc"
python3 - "$T" <<'EOF'
import struct, sys
t = sys.argv[1]
rsrc = b'RESOURCEFORK' * 10
fi = b'TEXTttxt' + bytes(8)
hdr = struct.pack('>II16xH', 0x00051607, 0x00020000, 2)
ents = struct.pack('>III', 9, 26 + 24, 32) + struct.pack('>III', 2, 26 + 24 + 32, len(rsrc))
open(t + '/._withrsrc', 'wb').write(hdr + ents + fi + bytes(16) + rsrc)
EOF
"$MB" "$I" PUT "$T/withrsrc" QUIET
"$MB" "$I" SYNCOUT "$D" QUIET
have "withrsrc" "data fork arrives"
have "._withrsrc" "sidecar arrives"
"$MB" "$I" SYNCIN "$D" QUIET
"$MB" "$I" LS | grep "withrsrc" | grep -q "rsrc=120" && ok "resource fork still on the Mac" || { bad "resource fork still on the Mac"; "$MB" "$I" LS | grep withrsrc; }
check 8

echo "== 9 names: a slash in a Mac name, a name too long for the Mac, a name with spaces and an umlaut"
echo "slash" > "$T/slash"
"$MB" "$I" PUT "$T/slash" AS "in/out" QUIET
"$MB" "$I" SYNCOUT "$D" QUIET
have "in_out" "a Mac name with a slash arrives as in_out"
O=$("$MB" "$I" SYNCIN "$D")
echo "$O" | grep -q "0 to the Mac, 0 from the Mac, 0 deleted" && ok "and is not copied back and forth" || { bad "and is not copied back and forth"; echo "$O" | tail -3; }
echo x > "$D/this name is far too long for a Macintosh.txt"
printf 'umlaut' > "$D/$(printf 'Gr\xfc\xdfe an alle.txt')"
O=$("$MB" "$I" SYNCIN "$D" 2>&1)
echo "$O" | grep -q "longer than 31" && ok "a long name is left out with a warning" || bad "a long name is left out with a warning"
"$MB" "$I" LS RAW | grep -q "an alle.txt" && ok "a name with umlauts is on the Mac" || bad "a name with umlauts is on the Mac"
O=$("$MB" "$I" SYNCIN "$D" 2>&1)
echo "$O" | grep -q "0 to the Mac, 0 from the Mac, 0 deleted" && ok "names are stable over a second sync" || { bad "names are stable over a second sync"; echo "$O" | tail -3; }
check 9

echo "== 10 the Mac did not shut down cleanly: its changes still arrive, the image is made anew"
echo "saved just before the crash" > "$T/crash.txt"
"$MB" "$I" PUT "$T/crash.txt" QUIET
python3 - "$I" <<'EOF2'
import sys
f = open(sys.argv[1], 'r+b'); f.seek(1024 + 10); a = bytearray(f.read(2)); a[0] &= 0xFE; f.seek(1024 + 10); f.write(a)
EOF2
"$MB" "$I" INFO | grep -qi "not.*clean\|mounted" && ok "image now counts as not shut down" || { bad "image now counts as not shut down"; "$MB" "$I" INFO | tail -3; }
O=$("$MB" "$I" SYNCIN "$D")
have "crash.txt" "the file from the crashed session is in the drawer"
echo "$O" | grep -q "New Mac disk" && ok "a new image is made" || { bad "a new image is made"; echo "$O" | tail -3; }
"$MB" "$I" LS | grep -q "crash.txt" && ok "and holds the file" || bad "and holds the file"
check 10

echo "== 11 an empty drawer, and a drawer that is emptied"
E=$T/Empty; mkdir "$E"
"$MB" "$T/e.hfv" SYNCIN "$E" QUIET && ok "empty drawer syncs" || bad "empty drawer syncs"
echo one > "$E/one.txt"; "$MB" "$T/e.hfv" SYNCIN "$E" QUIET
rm "$E/one.txt"; "$MB" "$T/e.hfv" SYNCIN "$E" QUIET
[ -z "$("$MB" "$T/e.hfv" LS)" ] && ok "emptied drawer empties the Mac disk" || { bad "emptied drawer empties the Mac disk"; "$MB" "$T/e.hfv" LS; }

echo "== 12 the Mac disk is erased on the Mac (or swapped): the drawer keeps everything"
S=$T/Safe; mkdir "$S"; for i in $(seq 1 30); do echo "file $i" > "$S/f$i.txt"; done
"$MB" "$T/s.hfv" SYNCIN "$S" QUIET
sleep 1; "$MB" "$T/s.hfv" CREATE 8 NAME Amiga FORCE QUIET          # a fresh, empty disk in its place
O=$("$MB" "$T/s.hfv" SYNCOUT "$S" 2>&1)
[ "$(ls "$S" | wc -l)" = 30 ] && ok "nothing deleted after the disk was made anew" || { bad "nothing deleted after the disk was made anew"; echo "$O" | tail -3; }
echo "$O" | grep -q "Nothing was deleted" && ok "and it says so" || { bad "and it says so"; echo "$O" | tail -3; }
"$MB" "$T/s.hfv" SYNCIN "$S" QUIET
[ "$("$MB" "$T/s.hfv" LS | wc -l)" = 30 ] && ok "the next SYNCIN puts the files on the Mac again" || bad "the next SYNCIN puts the files on the Mac again"

echo "== 13 most files vanish from the same disk: nothing is deleted either; a few are"
for i in $(seq 1 20); do "$MB" "$T/s.hfv" DELETE "f$i.txt" QUIET; done
O=$("$MB" "$T/s.hfv" SYNCOUT "$S" 2>&1)
[ "$(ls "$S" | wc -l)" = 30 ] && ok "20 of 30 missing: nothing deleted" || { bad "20 of 30 missing: nothing deleted"; echo "$O" | tail -3; }
"$MB" "$T/s.hfv" SYNCIN "$S" QUIET
for i in 1 2 3; do "$MB" "$T/s.hfv" DELETE "f$i.txt" QUIET; done
"$MB" "$T/s.hfv" SYNCOUT "$S" QUIET
[ "$(ls "$S" | wc -l)" = 27 ] && ok "3 of 30 deleted on the Mac: deleted here" || bad "3 of 30 deleted on the Mac: deleted here"

echo "== 14 the same image is given another drawer"
O2=$T/Other; mkdir "$O2"; echo "other drawer" > "$O2/other.txt"
echo "unsynced mac file" > "$T/unsynced.txt"; "$MB" "$T/s.hfv" PUT "$T/unsynced.txt" QUIET
O=$("$MB" "$T/s.hfv" SYNCOUT "$O2" 2>&1); [ $? = 10 ] && ok "SYNCOUT into another drawer is refused" || { bad "SYNCOUT into another drawer is refused"; echo "$O" | tail -2; }
"$MB" "$T/s.hfv" SYNCIN "$O2" QUIET
[ "$(ls "$O2" | wc -l)" = 1 ] && ok "nothing from the old image lands in the new drawer" || { bad "nothing from the old image lands in the new drawer"; ls "$O2"; }
[ "$("$MB" "$T/s.hfv" LS | wc -l)" = 1 ] && ok "the image now holds the new drawer only" || { bad "the image now holds the new drawer only"; "$MB" "$T/s.hfv" LS | head -4; }
[ "$(ls "$S" | wc -l)" = 27 ] && ok "the old drawer is untouched" || bad "the old drawer is untouched"
[ $fail = 0 ] && echo "ALL OK" || echo "SOME TESTS FAILED"
exit $fail
