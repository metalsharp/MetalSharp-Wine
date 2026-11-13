#!/bin/bash
# Tests metalsharp-vmd against a private socket, configuration and stand-ins for wine and wineserver.
#   test/vmd-test.sh path/to/metalsharp-vmd
# One Finder window is revealed (the fallback for a folder name Wine's explorer must not be given).
# No game and no real Wine is started: the stand-ins are shell scripts that record how they were run.
# (a bash script: run with zsh, it runs itself again with bash)
[ -z "${ZSH_VERSION:-}" ] || exec /bin/bash "$0" "$@"
VMD=${1:?usage: vmd-test.sh path/to/metalsharp-vmd}
T=$(cd "$(mktemp -d /tmp/vmdtest.XXXXXX)" && pwd -P)
trap 'kill $pid $(cat "$T"/share/steamapps/compatdata/*/pfx/game.pid "$T"/stubborn.pid "$T"/pid.* 2>/dev/null) 2>/dev/null; kill -9 $(cat "$T"/pid.* 2>/dev/null) 2>/dev/null; wait $pid 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/share/game/sub" "$T/share/bad,name" "$T/share/nul.d" "$T/outside" "$T/pfx"
touch "$T/share/game/file.txt" "$T/outside/secret"
ln -s "$T/outside" "$T/share/escape"
# The stand-in wine. For the file explorer it records one line. For a game it records its folder, its
# arguments and its whole environment, then behaves as the program's name says.
cat > "$T/wine" <<W
#!/bin/sh
T='$T'
W
cat >> "$T/wine" <<'W'
if [ "$1" = explorer.exe ]; then printf '%s\n' "$WINEPREFIX|$METALSHARP_EXPLORER_FOLDER_ONLY|$*" >> "$T/wine.log"; exit 0; fi
name=$(basename "$1" | tr A-Z a-z); name=${name%.exe}
{ echo "cwd=$(pwd -P)"; for a in "$@"; do printf 'arg=[%s]\n' "$a"; done; env | sort | sed 's/^/env:/'; } > "$T/launch.$STEAM_COMPAT_APP_ID.log"
echo "output of $name"
case $name in
  exit7)    exit 7;;
  killed)   kill -TERM $$;;
  sleeper)  echo $$ > "$WINEPREFIX/game.pid"; echo $$ > "$T/pid.$STEAM_COMPAT_APP_ID"; : > "$WINEPREFIX/server"; exec sleep 300;;   # a game: runs until stopped
  stubborn) echo $$ > "$T/stubborn.pid"; : > "$WINEPREFIX/server"; exec sleep 300;;        # one wineserver -k does not reach
  launcher) : > "$WINEPREFIX/server"; sleep 300 & echo $! > "$WINEPREFIX/game.pid"; echo $! > "$T/pid.$STEAM_COMPAT_APP_ID"; exit 0;;  # starts the game and exits
  deaf)     echo $$ > "$T/pid.$STEAM_COMPAT_APP_ID"; trap '' TERM; while :; do sleep 0.2; done;;  # does not end on SIGTERM
esac
exit 0
W
chmod +x "$T/wine"
# The stand-in wineserver: a prefix has a server for as long as its "server" file is there.
cat > "$T/wineserver" <<W
#!/bin/sh
echo "\$1 \$WINEPREFIX \$USER" >> '$T/wineserver.log'
case \$1 in
  -w) while [ -e "\$WINEPREFIX/server" ]; do sleep 0.1; done;;
  -k) kill \$(cat "\$WINEPREFIX/game.pid" 2>/dev/null) 2>/dev/null; rm -f "\$WINEPREFIX/server";;
esac
W
chmod +x "$T/wineserver"
cat > "$T/bridge.json" <<J
{"shared_roots": ["$T/share", "/nonexistent/root", 7], "file_manager": "wine",
 "wine_prefix": "$T/pfx", "wine": "$T/wine", "steam_command": "$T/steamcmd", "unknown": {"a": [1, "x", null]}}
J
printf '#!/bin/sh\nprintf "%%s\\n" "$*" >> "%s/steamcmd.log"\n' "$T" > "$T/steamcmd"; chmod +x "$T/steamcmd"
# (the variables are there to show that a game gets none of the daemon's own environment; LC_ALL=C is
# what metalsharp-steam starts the daemon with)
LD_PRELOAD=/nonexistent/x.so GRAPHICS_BACKEND=wine VMD_TEST_LEAK=1 LC_ALL=C METALSHARP_TEST=1 WINESERVER=/nonexistent \
  "$VMD" --socket "$T/s" --config "$T/bridge.json" 2> "$T/log" & pid=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$T/s" ] && break; sleep 0.2; done
fail=0
ask() { printf '%s\n' "$1" | nc -U "$T/s"; }
check() {  # check <name> <request> <expected reply substring>
  got=$(ask "$2")
  case "$got" in *"$3"*) echo "ok   $1";; *) echo "FAIL $1: got $got, wanted $3"; fail=1;; esac
}
check ping            '{"op": "ping"}' '{"ok": true}'
check malformed       'not json' 'malformed request'
check trailing        '{"op": "ping"} x' 'malformed request'
check unknown-op      '{"op": "reboot"}' 'unknown request'
check no-path         '{"op": "open-path"}' 'not an absolute path'
check relative        '{"op": "open-path", "path": "share/game"}' 'not an absolute path'
check control-char    '{"op": "open-path", "path": "'"$T"'/share/ga\u000ame"}' 'not an absolute path'
check outside         '{"op": "open-path", "path": "'"$T"'/outside"}' 'not inside a shared folder'
check guest-only      '{"op": "open-path", "path": "/home/steam/.local/share/Steam"}' 'not inside a shared folder'
check symlink-escape  '{"op": "open-path", "path": "'"$T"'/share/escape/secret"}' 'not inside a shared folder'
check dotdot-escape   '{"op": "open-path", "path": "'"$T"'/share/../outside"}' 'not inside a shared folder'
check no-oracle-1     '{"op": "open-path", "path": "'"$T"'/share/../outside/secret"}' 'not inside a shared folder'
check no-oracle-2     '{"op": "open-path", "path": "'"$T"'/share/../outside/nothing"}' 'not inside a shared folder'
check no-oracle-3     '{"op": "open-path", "path": "'"$T"'/share/escape/nothing"}' 'not inside a shared folder'
check unclean-op      '{"op": "ping\u0000x"}' 'unknown request'
check bare-sign       '{"op": "ping", "x": -}' '{"ok": true}'
check url-space       '{"op": "open-url", "url": "http://a b"}' 'not a usable URL'
check url-badhost     '{"op": "open-url", "url": "http://@x"}' 'only http and https'
check missing         '{"op": "open-path", "path": "'"$T"'/share/nothing"}' 'no such file or folder'
check url-scheme      '{"op": "open-url", "url": "file:///etc/passwd"}' 'only http and https'
check url-nohost      '{"op": "open-url", "url": "https:///x"}' 'only http and https'
check url-missing     '{"op": "open-url"}' 'not a usable URL'
check folder          '{"op": "open-path", "path": "'"$T"'/share/game"}' '{"ok": true}'
check throttled       '{"op": "open-path", "path": "'"$T"'/share/game"}' 'too many requests'
sleep 0.6
check file-in-folder  '{"op": "open-path", "path": "'"$T"'/share/game/file.txt", "extra": [1, {"k": "v"}]}' '{"ok": true}'
sleep 0.6
check escaped-string  '{"op": "open-path", "path": "'"$T"'\/share\/game\/sub"}' '{"ok": true}'
sleep 0.6
check unsafe-name     '{"op": "open-path", "path": "'"$T"'/share/bad,name"}' '{"ok": true}'
sleep 0.5
win=$(printf 'Z:%s' "$T/share/game" | tr / '\\')
want="$T/pfx|1|explorer.exe $win
$T/pfx|1|explorer.exe $win
$T/pfx|1|explorer.exe $win\\sub"
if [ "$(cat "$T/wine.log")" = "$want" ]; then echo "ok   wine invocations"; else echo "FAIL wine invocations:"; cat "$T/wine.log"; fail=1; fi
# a silent client is answered and dropped at the deadline, and a second daemon on the same socket stands down
start=$(date +%s); got=$( (sleep 8) | nc -U "$T/s" | { read -r line; echo "$(( $(date +%s) - start )) $line"; })
case "$got" in [456]" "*"malformed request"*) echo "ok   deadline (${got%% *}s)";; *) echo "FAIL deadline: $got"; fail=1;; esac
"$VMD" --socket "$T/s" --config "$T/bridge.json" 2>&1 | grep -q "already running" && echo "ok   single instance" || { echo "FAIL single instance"; fail=1; }
check still-up        '{"op": "ping"}' '{"ok": true}'
sleep 0.6
check choose-library  '{"op": "choose-library", "path": "/ignored"}' '{"ok": true}'
sleep 0.3
[ "$(cat "$T/steamcmd.log" 2>/dev/null)" = "library choose" ] && echo "ok   choose-library command" || { echo "FAIL choose-library command"; fail=1; }
[ "$(stat -f %Lp "$T/s")" = 600 ] && echo "ok   socket mode" || { echo "FAIL socket mode"; fail=1; }
# ---- Play: games started for Linux Steam ----
L=$T/share/steamapps; G="$L/common/My Game"
mkdir -p "$G/bin" "$G/dir.exe" "$T/outside/steamapps/compatdata/7" "$L/compatdata/200"
for n in 100 101 102 103 104 105; do mkdir -p "$L/compatdata/$n"; done
touch "$G/game.exe" "$G/GAME2.EXE" "$G/exit7.exe" "$G/killed.exe" "$G/sleeper.exe" "$G/stubborn.exe" "$G/launcher.exe" "$G/deaf.exe" \
      "$G/notes.txt" "$T/outside/evil.exe"
ln -s "$T/outside" "$L/compatdata/200/pfx"
ln -s "$T/outside/steamapps/compatdata/7" "$L/compatdata/8"
ln -s "$T/outside/evil.exe" "$G/link.exe"
rq() {  # rq <exe> <appid> [more members]: a launch request for a program of the game folder
  printf '{"op":"launch","exe":"%s","appid":"%s","compat_data":"%s/compatdata/%s"%s}' "$G/$1" "$2" "$L" "$2" "${3:+,$3}"
}
# a connection that stays open: conn <n> <request>, say <n> <line>, drop <n>, line <n> <k> (its k-th reply)
conn() {
  rm -f "$T/c$1.in" "$T/c$1.out"; mkfifo "$T/c$1.in"
  nc -U "$T/s" < "$T/c$1.in" > "$T/c$1.out" & eval "nc$1=$!"
  eval "exec $1>\"\$T/c$1.in\""
  printf '%s\n' "$2" >&$1
}
say() { printf '%s\n' "$2" >&$1; }
drop() { eval "kill \$nc$1; wait \$nc$1 2>/dev/null; exec $1>&-"; }
line() {
  local i=0
  while [ "$(wc -l < "$T/c$1.out" | tr -d ' ')" -lt "$2" ] && [ $i -lt ${3:-80} ]; do sleep 0.1; i=$((i+1)); done
  sed -n "$2p" "$T/c$1.out"
}
expect() {  # expect <name> <got> <wanted substring>
  case "$2" in *"$3"*) echo "ok   $1";; *) echo "FAIL $1: got $2, wanted $3"; fail=1;; esac
}
has() {  # has <name> <file> <exact line>
  grep -qxF -- "$3" "$2" && echo "ok   $1" || { echo "FAIL $1: no line $3 in"; cat "$2"; fail=1; }
}
hasnot() {  # hasnot <name> <file> <pattern>
  grep -q -- "$3" "$2" && { echo "FAIL $1:"; grep -- "$3" "$2"; fail=1; } || echo "ok   $1"
}
gone() {  # gone <name> <pid>: the process ends within two seconds
  local i=0
  while kill -0 "$2" 2>/dev/null && [ $i -lt 20 ]; do sleep 0.1; i=$((i+1)); done
  kill -0 "$2" 2>/dev/null && { echo "FAIL $1: process $2 is still there"; fail=1; } || echo "ok   $1"
}
STARTED='{"ok": true, "state": "started"}'

check launch-off      "$(rq game.exe 100)" 'launching games is turned off'
cat > "$T/bridge.json" <<J
{"shared_roots": ["$T/share"], "file_manager": "wine", "wine_prefix": "$T/pfx", "wine": "$T/wine",
 "launch": "on", "launch_log": "$T/game.log", "lang": "de_DE.UTF-8"}
J
# refusals: none of them starts anything, and none says whether a path outside the shares exists
E='not an .exe file inside a shared folder'; D='compatibility data folder is not usable'
check exe-missing     '{"op":"launch","appid":"100","compat_data":"'"$L"'/compatdata/100"}' "$E"
check exe-relative    '{"op":"launch","exe":"game.exe","appid":"100","compat_data":"'"$L"'/compatdata/100"}' "$E"
check exe-not-there   "$(rq nothing.exe 100)" "$E"
check exe-not-exe     "$(rq notes.txt 100)" "$E"
check exe-folder      "$(rq dir.exe 100)" "$E"
check exe-link-out    "$(rq link.exe 100)" "$E"
check exe-outside     '{"op":"launch","exe":"'"$T"'/outside/evil.exe","appid":"100","compat_data":"'"$L"'/compatdata/100"}' "$E"
check exe-outside-2   '{"op":"launch","exe":"'"$T"'/outside/nothing.exe","appid":"100","compat_data":"'"$L"'/compatdata/100"}' "$E"
check exe-unclean     '{"op":"launch","exe":"'"$G"'/game\u0007.exe","appid":"100","compat_data":"'"$L"'/compatdata/100"}' 'not a usable launch request'
check cwd-outside     "$(rq game.exe 100 '"cwd":"'"$T"'/outside"')" 'working folder is not inside a shared folder'
check cwd-file        "$(rq game.exe 100 '"cwd":"'"$G"'/notes.txt"')" 'working folder is not inside a shared folder'
check cwd-unclean     "$(rq game.exe 100 '"cwd":"'"$G"'\nx"')" 'not a usable launch request'
check data-missing    '{"op":"launch","exe":"'"$G"'/game.exe","appid":"100"}' "$D"
check data-not-there  "$(rq game.exe 999)" "$D"
check data-outside    '{"op":"launch","exe":"'"$G"'/game.exe","appid":"7","compat_data":"'"$T"'/outside/steamapps/compatdata/7"}' "$D"
check data-link-out   "$(rq game.exe 8)" "$D"
check data-shape      '{"op":"launch","exe":"'"$G"'/game.exe","appid":"100","compat_data":"'"$G"'"}' "$D"
check data-shape-2    '{"op":"launch","exe":"'"$G"'/game.exe","appid":"100","compat_data":"'"$L"'/compatdata"}' "$D"
check pfx-link        "$(rq game.exe 200)" "$D"
sleep 1.1   # (that one got as far as taking a place; the others were refused before the interval between launches)
check appid-missing   '{"op":"launch","exe":"'"$G"'/game.exe","compat_data":"'"$L"'/compatdata/100"}' 'not a usable app id'
check appid-letters   '{"op":"launch","exe":"'"$G"'/game.exe","appid":"12a","compat_data":"'"$L"'/compatdata/100"}' 'not a usable app id'
check appid-long      '{"op":"launch","exe":"'"$G"'/game.exe","appid":"12345678901","compat_data":"'"$L"'/compatdata/100"}' 'not a usable app id'
check gameid-long     "$(rq game.exe 100 '"gameid":"123456789012345678901"')" 'not a usable app id'
check gameid-sign     "$(rq game.exe 100 '"gameid":"-1"')" 'not a usable app id'
check backend-unknown "$(rq game.exe 100 '"backend":"metal"')" 'unknown graphics backend'
check backend-path    "$(rq game.exe 100 '"backend":"../dxvk"')" 'unknown graphics backend'
check args-string     "$(rq game.exe 100 '"args":"-x"')" 'too many or unusable arguments'
check args-number     "$(rq game.exe 100 '"args":[1]')" 'too many or unusable arguments'
check args-nested     "$(rq game.exe 100 '"args":[["-x"]]')" 'too many or unusable arguments'
check args-unclean    "$(rq game.exe 100 '"args":["a\u0000b"]')" 'too many or unusable arguments'
many=$(printf '"a",%.0s' $(seq 65)); check args-65 "$(rq game.exe 100 '"args":['"${many%,}"']')" 'too many or unusable arguments'
big=$(head -c 6000 /dev/zero | tr '\0' a); check args-17k "$(rq game.exe 100 '"args":["'$big'","'$big'","'$big'"]')" 'too many or unusable arguments'
[ ! -e "$T/launch.100.log" ] && [ ! -e "$T/wineserver.log" ] && echo "ok   refusals start nothing" || { echo "FAIL a refused launch ran something"; fail=1; }
[ ! -e "$L/compatdata/999" ] && [ ! -e "$T/outside/steamapps/compatdata/7/pfx" ] && echo "ok   refusals create nothing" || { echo "FAIL a refused launch created a folder"; fail=1; }

# an accepted launch: Wine's default backend, arguments with spaces, quotes and things a shell would expand
cat > "$T/args.json" <<'A'
"args":["-a","two words","q\"uote'","back\\slash","\u00fcn\u00ef \ud83c\udfae","","$HOME;`id` %s","--x=\/y"]
A
conn 3 "$(rq game.exe 100 "$(cat "$T/args.json")")"
expect launch-started "$(line 3 1)" "$STARTED"
expect launch-exit-0  "$(line 3 2)" '{"ok": true, "exit": 0}'
F=$T/launch.100.log
cat > "$T/want" <<A
cwd=$G
arg=[$G/game.exe]
arg=[-a]
arg=[two words]
arg=[q"uote']
arg=[back\\slash]
arg=[ünï 🎮]
arg=[]
arg=[\$HOME;\`id\` %s]
arg=[--x=/y]
A
[ "$(grep -v '^env:' "$F")" = "$(cat "$T/want")" ] && echo "ok   launch argv and folder" || { echo "FAIL launch argv and folder:"; grep -v '^env:' "$F"; fail=1; }
has    env-prefix     "$F" "env:WINEPREFIX=$L/compatdata/100/pfx"
has    env-user       "$F" 'env:USER=steamuser'
has    env-logname    "$F" 'env:LOGNAME=steamuser'
has    env-home       "$F" "env:HOME=$HOME"
has    env-path       "$F" 'env:PATH=/usr/bin:/bin:/usr/sbin:/sbin'
has    env-lang       "$F" 'env:LANG=de_DE.UTF-8'
has    env-winedebug  "$F" 'env:WINEDEBUG=-all'
has    env-appid      "$F" 'env:SteamAppId=100'
has    env-gameid     "$F" 'env:SteamGameId=100'
has    env-compat-id  "$F" 'env:STEAM_COMPAT_APP_ID=100'
has    env-compat     "$F" "env:STEAM_COMPAT_DATA_PATH=$L/compatdata/100"
grep -q '^env:TMPDIR=/' "$F" && echo "ok   env-tmpdir" || { echo "FAIL env-tmpdir"; fail=1; }
hasnot env-no-backend "$F" '^env:GRAPHICS_BACKEND='
# the backend asked for, which Wine puts back over a game folder's launch.env: there for every backend
has    env-marker     "$F" 'env:METALSHARP_LAUNCH_BACKEND=mullion'
# (DYLD_ variables cannot be looked for this way: macOS takes them away from /bin/sh, which the stand-in is)
hasnot env-no-ld      "$F" '^env:LD_'
hasnot env-no-leak    "$F" '^env:\(VMD_TEST_LEAK\|LC_ALL\|METALSHARP_TEST\|WINESERVER\|WINEDLL\)'
[ "$(grep -c '^env:METALSHARP_' "$F")" = 1 ] && echo "ok   env-one-metalsharp" || { echo "FAIL env-one-metalsharp:"; grep '^env:METALSHARP_' "$F"; fail=1; }
[ -d "$L/compatdata/100/pfx" ] && [ -z "$(ls -A "$L/compatdata/100/pfx")" ] && echo "ok   prefix folder made, left empty" || { echo "FAIL prefix folder"; fail=1; }
has    waited-for-server "$T/wineserver.log" "-w $L/compatdata/100/pfx steamuser"
sleep 1.1

# another backend, a folder and a game id of its own, an .EXE, and a symlinked path that is resolved
ln -s "$G" "$T/share/alias"
conn 3 '{"op":"launch","backend":"dxvk","exe":"'"$T"'/share/alias/GAME2.EXE","cwd":"'"$T"'/share/alias/bin","appid":"100","gameid":"13835058055282163712","compat_data":"'"$L"'/compatdata/100","args":[]}'
expect dxvk-started   "$(line 3 1)" "$STARTED"
expect dxvk-exit      "$(line 3 2)" '{"ok": true, "exit": 0}'
has    dxvk-backend   "$F" 'env:GRAPHICS_BACKEND=dxvk'
has    dxvk-marker    "$F" 'env:METALSHARP_LAUNCH_BACKEND=dxvk'
has    dxvk-gameid    "$F" 'env:SteamGameId=13835058055282163712'
has    dxvk-cwd       "$F" "cwd=$G/bin"
has    dxvk-exe       "$F" "arg=[$G/GAME2.EXE]"
sleep 1.1
conn 3 "$(rq game.exe 100 '"backend":"wined3d"')"
expect wined3d-exit   "$(line 3 2)" '{"ok": true, "exit": 0}'
has    wined3d-backend "$F" 'env:GRAPHICS_BACKEND=wine'
has    wined3d-marker "$F" 'env:METALSHARP_LAUNCH_BACKEND=wine'
sleep 1.1
# every backend's name: what Wine is asked for (no GRAPHICS_BACKEND is Mullion, its default), and the marker
for b in mullion: dxmt:dxmt:mullion dxvk:dxvk vkd3d:vkd3d wine:wine d3dmetal:d3dmetal:wine; do
  name=${b%%:*}; rest=${b#*:}; value=${rest%%:*}; marker=${rest#*:}; [ "$rest" = "$value" ] && marker=${value:-$name}
  conn 3 "$(rq game.exe 100 '"backend":"'"$name"'"')"
  expect "$name-exit" "$(line 3 2)" '{"ok": true, "exit": 0}'
  if [ -n "$value" ]; then has "$name-backend" "$F" "env:GRAPHICS_BACKEND=$value"; else hasnot "$name-no-backend" "$F" '^env:GRAPHICS_BACKEND='; fi
  has "$name-marker" "$F" "env:METALSHARP_LAUNCH_BACKEND=$marker"
  [ "$(grep -c '^env:METALSHARP_LAUNCH_BACKEND=' "$F")" = 1 ] && echo "ok   $name-marker-once" || { echo "FAIL $name-marker-once"; fail=1; }
  sleep 1.1
done

# exit status, the output file
conn 3 "$(rq exit7.exe 100)"
expect exit-status    "$(line 3 2)" '{"ok": true, "exit": 7}'
[ "$(cat "$T/game.log")" = "output of exit7" ] && echo "ok   output file, truncated per launch" || { echo "FAIL output file: $(cat "$T/game.log")"; fail=1; }
[ "$(stat -f %Lp "$T/game.log")" = 600 ] && echo "ok   output file mode" || { echo "FAIL output file mode"; fail=1; }
sleep 1.1
conn 3 "$(rq killed.exe 100)"
expect exit-signal    "$(line 3 2)" '{"ok": true, "exit": 143}'
sleep 1.1
# an output file that is a link is not followed; the game runs without one
rm -f "$T/game.log"; ln -s "$T/outside/secret" "$T/game.log"
conn 3 "$(rq game.exe 100)"
expect log-link-runs  "$(line 3 2)" '{"ok": true, "exit": 0}'
[ ! -s "$T/outside/secret" ] && echo "ok   output link not followed" || { echo "FAIL output written through a link"; fail=1; }
rm -f "$T/game.log"
sleep 1.1

# Stop ends the game; a second launch into its prefix is refused while a guest is attached
conn 3 "$(rq sleeper.exe 101)"
expect stop-started   "$(line 3 1)" "$STARTED"
check too-fast        "$(rq game.exe 100)" 'too many requests'
sleep 0.5; game=$(cat "$L/compatdata/101/pfx/game.pid")
check already-running "$(rq game.exe 101)" 'this game is already running'
[ "$(wc -l < "$T/c3.out" | tr -d ' ')" = 1 ] && kill -0 "$game" && echo "ok   running game holds its connection" || { echo "FAIL running game"; fail=1; }
say 3 'noise'; say 3 '{"op": "ping"}'; sleep 0.5
kill -0 "$game" 2>/dev/null && echo "ok   other lines do not stop it" || { echo "FAIL a line that is not stop ended the game"; fail=1; }
say 3 '{"op": "stop"}'
expect stop-exit      "$(line 3 2)" '{"ok": true, "exit": 143}'
gone   stop-ended-game "$game"
has    stop-used-wineserver "$T/wineserver.log" "-k $L/compatdata/101/pfx steamuser"
sleep 1.1
# what wineserver -k does not reach is killed
conn 3 "$(rq stubborn.exe 102)"
expect stubborn-started "$(line 3 1)" "$STARTED"
sleep 0.5; game=$(cat "$T/stubborn.pid")
say 3 '{"op":"stop"}'
expect stubborn-exit  "$(line 3 2)" '{"ok": true, "exit": 137}'
gone   stubborn-killed "$game"
sleep 1.1

# Stop pressed at once: the stop line arrives together with the request and still ends the game
conn 3 "$(rq sleeper.exe 105)
{\"op\":\"stop\"}"
expect early-stop-started "$(line 3 1)" "$STARTED"
case "$(line 3 2)" in '{"ok": true, "exit": 137}'|'{"ok": true, "exit": 143}') echo "ok   early-stop-exit";; *) echo "FAIL early-stop-exit: got $(line 3 2)"; fail=1;; esac
sleep 1.1

# a connection that drops does not end the game, and the next launch for it takes it over
conn 3 "$(rq sleeper.exe 103)"
expect drop-started   "$(line 3 1)" "$STARTED"
sleep 0.5; game=$(cat "$L/compatdata/103/pfx/game.pid")
drop 3; sleep 1
kill -0 "$game" 2>/dev/null && echo "ok   dropped connection keeps the game" || { echo "FAIL the game ended with its connection"; fail=1; }
grep -q 'connection is gone; the game is kept' "$T/log" && echo "ok   drop logged" || { echo "FAIL drop not logged"; fail=1; }
conn 4 "$(rq sleeper.exe 103)"
expect rejoin-started "$(line 4 1)" "$STARTED"
sleep 0.5
[ "$(cat "$L/compatdata/103/pfx/game.pid")" = "$game" ] && echo "ok   rejoin starts no second copy" || { echo "FAIL rejoin started another copy"; fail=1; }
say 4 '{"op":"stop"}'
expect rejoin-stop    "$(line 4 2)" '{"ok": true, "exit": 143}'
gone   rejoin-ended-game "$game"
sleep 1.1

# Stop pressed at once on a game that is taken over: the stop line behind the joining request ends it
conn 3 "$(rq sleeper.exe 103)"
expect join-stop-started "$(line 3 1)" "$STARTED"
sleep 0.5; game=$(cat "$L/compatdata/103/pfx/game.pid")
drop 3; sleep 1.1
conn 4 "$(rq sleeper.exe 103)
{\"op\":\"stop\"}"
expect join-stop-joined "$(line 4 1)" "$STARTED"
expect join-stop-exit "$(line 4 2)" '{"ok": true, "exit": 143}'
gone   join-stop-ended-game "$game"
sleep 1.1

# "detach" (metalsharp-steam stop, before the virtual machine goes down): the connection is let go of, a
# stop line sent on it afterwards ends nothing, and the next launch takes the game over
check detach-none     '{"op": "detach"}' '{"ok": true, "games": 0}'
conn 3 "$(rq sleeper.exe 103)"
expect detach-started "$(line 3 1)" "$STARTED"
sleep 0.5; game=$(cat "$L/compatdata/103/pfx/game.pid")
check detach-one      '{"op": "detach"}' '{"ok": true, "games": 1}'
say 3 '{"op":"stop"}' 2>/dev/null; sleep 1
kill -0 "$game" 2>/dev/null && echo "ok   detached game is kept, stop or not" || { echo "FAIL the game ended after detach"; fail=1; }
[ "$(wc -l < "$T/c3.out" | tr -d ' ')" = 1 ] && echo "ok   detached connection gets no more replies" || { echo "FAIL detached connection: $(cat "$T/c3.out")"; fail=1; }
drop 3 2>/dev/null
grep -q 'let go of the guest.s connection' "$T/log" && echo "ok   detach logged" || { echo "FAIL detach not logged"; fail=1; }
check detach-kept     '{"op": "detach"}' '{"ok": true, "games": 1}'
conn 4 "$(rq sleeper.exe 103)"
expect detach-rejoin  "$(line 4 1)" "$STARTED"
[ "$(cat "$L/compatdata/103/pfx/game.pid")" = "$game" ] && echo "ok   detach-rejoin starts no second copy" || { echo "FAIL detach-rejoin started another copy"; fail=1; }
check join-too-fast   "$(rq sleeper.exe 100)" 'too many requests'
say 4 '{"op":"stop"}'
expect detach-rejoin-stop "$(line 4 2)" '{"ok": true, "exit": 143}'
gone   detach-ended-game "$game"
sleep 1.1

# The compatibility data folder swapped for a link out of the shares while requests come in: no "pfx" is
# ever made outside, and no game is started in a prefix outside. (The swap is two renames, so some
# requests find no folder at all; those are refused like any other.)
mkdir -p "$L/compatdata/300" "$T/outside/steamapps/compatdata/300"
ln -s "$T/outside/steamapps/compatdata/300" "$L/compatdata/300.link"
( while [ -e "$T/swapping" ]; do
    mv "$L/compatdata/300" "$L/compatdata/300.dir" 2>/dev/null && mv "$L/compatdata/300.link" "$L/compatdata/300" &&
    mv "$L/compatdata/300" "$L/compatdata/300.link" && mv "$L/compatdata/300.dir" "$L/compatdata/300"
  done ) & : > "$T/swapping"; swapper=$!
for i in $(seq 1 40); do ask "$(rq game.exe 300)" > /dev/null; sleep 0.26; done
rm -f "$T/swapping"; wait $swapper 2>/dev/null
[ ! -e "$T/outside/steamapps/compatdata/300/pfx" ] && echo "ok   swapped data folder: nothing made outside" || { echo "FAIL a pfx folder was made outside the shares"; fail=1; }
hasnot swapped-data-prefix "$T/launch.300.log" "^env:WINEPREFIX=$T/outside" 2>/dev/null
sleep 1.1

# what the guest can cause at will is not written to the log without limit
before=$(wc -l < "$T/log")
for i in $(seq 1 80); do ask '{"op": "reboot"}' > /dev/null; done
after=$(wc -l < "$T/log")
[ $((after - before)) -le 25 ] && echo "ok   refusals in the log are limited ($((after - before)) lines for 80)" || { echo "FAIL log grew by $((after - before)) lines for 80 refusals"; fail=1; }

# a launcher that exits is followed to the end of what it started
conn 3 "$(rq launcher.exe 104)"
expect launcher-started "$(line 3 1)" "$STARTED"
sleep 1.5
[ "$(wc -l < "$T/c3.out" | tr -d ' ')" = 1 ] && echo "ok   launcher exit is not the game's end" || { echo "FAIL launch reported over while the game runs"; fail=1; }
kill "$(cat "$L/compatdata/104/pfx/game.pid")"; rm -f "$L/compatdata/104/pfx/server"
expect launcher-exit  "$(line 3 2)" '{"ok": true, "exit": 0}'
sleep 1.1

# four games at once, and no fifth
for n in 1 2 3 4; do conn $((n + 2)) "$(rq sleeper.exe 10$n)"; expect "game-$n-started" "$(line $((n + 2)) 1)" "$STARTED"; sleep 1.1; done
check fifth-refused   "$(rq sleeper.exe 105)" 'too many games running'
check ping-meanwhile  '{"op": "ping"}' '{"ok": true}'
for n in 1 2 3 4; do say $((n + 2)) '{"op":"stop"}'; done
for n in 1 2 3 4; do expect "game-$n-stopped" "$(line $((n + 2)) 2)" '{"ok": true, "exit": 143}'; done
conn 3 "$(rq game.exe 105)"
expect room-again     "$(line 3 2)" '{"ok": true, "exit": 0}'

# ---- MetalSharp's Play: the launch intent the MetalSharp application leaves in the run folder ----
# (the folder the daemon's socket is in, which the guest cannot reach; here that is $T)
I=$T/launch-intent; mkdir -m 700 "$I"
sleep 1.1
printf 'dxvk\n' > "$I/100"
conn 3 "$(rq game.exe 100 '"backend":"mullion"')"
expect intent-exit    "$(line 3 2)" '{"ok": true, "exit": 0}'
has    intent-backend "$F" 'env:GRAPHICS_BACKEND=dxvk'
has    intent-marker  "$F" 'env:METALSHARP_LAUNCH_BACKEND=dxvk'
[ ! -e "$I/100" ] && echo "ok   intent used once (removed)" || { echo "FAIL intent file left behind"; fail=1; }
# the per-game prefix and Windows user stay as they were without a shared prefix
has    intent-pergame-prefix "$F" "env:WINEPREFIX=$L/compatdata/100/pfx"
has    intent-pergame-user   "$F" 'env:USER=steamuser'
grep -q 'MetalSharp asked for dxvk (Steam.s tool: mullion)' "$T/log" && echo "ok   intent logged" || { echo "FAIL intent not logged"; fail=1; }
sleep 1.1
# the next launch has no intent: Steam's tool again
conn 3 "$(rq game.exe 100 '"backend":"vkd3d"')"
expect no-intent-exit "$(line 3 2)" '{"ok": true, "exit": 0}'
has    no-intent-backend "$F" 'env:GRAPHICS_BACKEND=vkd3d'
sleep 1.1
# Mullion asked for in MetalSharp over Steam's DXVK tool: no GRAPHICS_BACKEND, the marker says mullion
printf 'mullion' > "$I/100"
conn 3 "$(rq game.exe 100 '"backend":"dxvk"')"
expect intent-mullion-exit "$(line 3 2)" '{"ok": true, "exit": 0}'
hasnot intent-mullion-no-backend "$F" '^env:GRAPHICS_BACKEND='
has    intent-mullion-marker "$F" 'env:METALSHARP_LAUNCH_BACKEND=mullion'
sleep 1.1
printf 'wine\n' > "$I/100"
conn 3 "$(rq game.exe 100 '"backend":"dxvk"')"
expect intent-wine-exit "$(line 3 2)" '{"ok": true, "exit": 0}'
has    intent-wine-backend "$F" 'env:GRAPHICS_BACKEND=wine'
has    intent-wine-marker "$F" 'env:METALSHARP_LAUNCH_BACKEND=wine'
sleep 1.1
# too old (over two minutes): ignored, and removed
printf 'vkd3d\n' > "$I/100"; touch -t "$(date -r $(( $(date +%s) - 200 )) +%Y%m%d%H%M.%S)" "$I/100"
conn 3 "$(rq game.exe 100 '"backend":"dxvk"')"
expect stale-intent-exit "$(line 3 2)" '{"ok": true, "exit": 0}'
has    stale-intent-ignored "$F" 'env:GRAPHICS_BACKEND=dxvk'
[ ! -e "$I/100" ] && echo "ok   stale intent removed" || { echo "FAIL stale intent left behind"; fail=1; }
sleep 1.1
# another game's intent: not this launch's, and kept for that game
printf 'vkd3d\n' > "$I/101"
conn 3 "$(rq game.exe 100 '"backend":"dxvk"')"
expect other-appid-exit "$(line 3 2)" '{"ok": true, "exit": 0}'
has    other-appid-ignored "$F" 'env:GRAPHICS_BACKEND=dxvk'
[ -f "$I/101" ] && echo "ok   other game's intent kept" || { echo "FAIL another game's intent was removed"; fail=1; }
rm -f "$I/101"
sleep 1.1
# a link is not followed
printf 'vkd3d\n' > "$T/outside/intent"; ln -s "$T/outside/intent" "$I/100"
conn 3 "$(rq game.exe 100 '"backend":"dxvk"')"
expect link-intent-exit "$(line 3 2)" '{"ok": true, "exit": 0}'
has    link-intent-ignored "$F" 'env:GRAPHICS_BACKEND=dxvk'
[ -f "$T/outside/intent" ] && echo "ok   link target untouched" || { echo "FAIL the link's target was removed"; fail=1; }
rm -f "$I/100"
sleep 1.1
# not a backend's name
printf 'metal\n' > "$I/100"
conn 3 "$(rq game.exe 100 '"backend":"dxvk"')"
expect bad-intent-exit "$(line 3 2)" '{"ok": true, "exit": 0}'
has    bad-intent-ignored "$F" 'env:GRAPHICS_BACKEND=dxvk'
[ ! -e "$I/100" ] && echo "ok   unusable intent removed" || { echo "FAIL unusable intent left behind"; fail=1; }
sleep 1.1
# a file of another user's (a hard link to a file root owns, where the system allows one): not used
rm -f "$I/100"
if ln /private/etc/hosts "$I/100" 2>/dev/null || ln /private/etc/shells "$I/100" 2>/dev/null; then
  conn 3 "$(rq game.exe 100 '"backend":"dxvk"')"
  expect foreign-intent-exit "$(line 3 2)" '{"ok": true, "exit": 0}'
  has    foreign-intent-ignored "$F" 'env:GRAPHICS_BACKEND=dxvk'
  grep -q "not a file of this user's" "$T/log" && echo "ok   foreign intent refused as such" || { echo "FAIL foreign intent: no log line"; fail=1; }
  rm -f "$I/100"
  sleep 1.1
else
  echo "skip foreign-owned intent (no file of another user's could be linked here)"
fi
# a folder others can write to: nothing in it is used
chmod 777 "$I"; printf 'vkd3d\n' > "$I/100"
conn 3 "$(rq game.exe 100 '"backend":"dxvk"')"
expect open-folder-exit "$(line 3 2)" '{"ok": true, "exit": 0}'
has    open-folder-ignored "$F" 'env:GRAPHICS_BACKEND=dxvk'
chmod 700 "$I"; rm -f "$I/100"
sleep 1.1

# ---- one prefix for every game ("game_prefix": MetalSharp's Steam prefix, shared with its other programs) ----
SP=$T/shared-pfx; mkdir -p "$SP"
for n in 400 401 402 403 404 405 406; do mkdir -p "$L/compatdata/$n"; done
cat > "$T/bridge.json" <<J
{"shared_roots": ["$T/share"], "file_manager": "wine", "wine_prefix": "$T/pfx", "wine": "$T/wine",
 "launch": "on", "launch_log": "$T/game.log", "game_prefix": "$SP", "game_user": "mac.user-1"}
J
# another program of the prefix (Windows Steam, say) keeps its wineserver running: "wineserver -w" would
# never return, and "wineserver -k" would end that program too
: > "$SP/server"; sleep 300 & other=$!; echo $other > "$T/pid.other"
: > "$T/wineserver.log"
conn 3 "$(rq game.exe 400)"
expect shared-started "$(line 3 1)" "$STARTED"
expect shared-ended   "$(line 3 2 50)" '{"ok": true, "exit": 0}'
F=$T/launch.400.log
has    shared-prefix  "$F" "env:WINEPREFIX=$SP"
has    shared-user    "$F" 'env:USER=mac.user-1'
has    shared-logname "$F" 'env:LOGNAME=mac.user-1'
has    shared-compat  "$F" "env:STEAM_COMPAT_DATA_PATH=$L/compatdata/400"
[ ! -e "$L/compatdata/400/pfx" ] && echo "ok   shared: no pfx made" || { echo "FAIL shared: a pfx folder was made"; fail=1; }
[ ! -s "$T/wineserver.log" ] && echo "ok   shared: the wineserver is not waited for" || { echo "FAIL shared: wineserver run:"; cat "$T/wineserver.log"; fail=1; }
sleep 1.1
# a launcher that exits is followed through its process group
conn 3 "$(rq launcher.exe 401)"
expect shared-launcher-started "$(line 3 1)" "$STARTED"
sleep 1.5
[ "$(wc -l < "$T/c3.out" | tr -d ' ')" = 1 ] && echo "ok   shared: launcher exit is not the game's end" || { echo "FAIL shared: launch over while its group runs"; fail=1; }
kill "$(cat "$T/pid.401")"
expect shared-launcher-exit "$(line 3 2)" '{"ok": true, "exit": 0}'
sleep 1.1
# two games at once in the prefix; Stop ends the one it is for, and nothing else of the prefix
conn 3 "$(rq sleeper.exe 402)"; expect shared-a-started "$(line 3 1)" "$STARTED"; sleep 1.1
conn 4 "$(rq sleeper.exe 403)"; expect shared-b-started "$(line 4 1)" "$STARTED"
sleep 0.5; a=$(cat "$T/pid.402"); b=$(cat "$T/pid.403")
say 3 '{"op":"stop"}'
expect shared-a-stop  "$(line 3 2)" '{"ok": true, "exit": 143}'
gone   shared-a-ended "$a"
kill -0 "$b" 2>/dev/null && echo "ok   shared: the other game runs on" || { echo "FAIL shared: Stop ended the other game"; fail=1; }
kill -0 "$other" 2>/dev/null && echo "ok   shared: the prefix's other program runs on" || { echo "FAIL shared: Stop ended another program of the prefix"; fail=1; }
hasnot shared-no-wineserver-k "$T/wineserver.log" '^-k'
# the other game: dropped (the virtual machine restarted), kept, and taken over by its next Play
drop 4; sleep 1
kill -0 "$b" 2>/dev/null && echo "ok   shared: dropped connection keeps the game" || { echo "FAIL shared: the game ended with its connection"; fail=1; }
sleep 0.2
check shared-detach   '{"op": "detach"}' '{"ok": true, "games": 1}'
conn 4 "$(rq sleeper.exe 403)"
expect shared-rejoin  "$(line 4 1)" "$STARTED"
sleep 0.5
[ "$(cat "$T/pid.403")" = "$b" ] && echo "ok   shared: rejoin starts no second copy" || { echo "FAIL shared: rejoin started another copy"; fail=1; }
sleep 0.7
check shared-same-game "$(rq sleeper.exe 403)" 'this game is already running'
sleep 1.1
# another game in the same prefix is a game of its own
conn 5 "$(rq sleeper.exe 404)"; expect shared-other-appid "$(line 5 1)" "$STARTED"
sleep 0.5; c=$(cat "$T/pid.404")
say 4 '{"op":"stop"}'; say 5 '{"op":"stop"}'
expect shared-b-stop  "$(line 4 2)" '{"ok": true, "exit": 143}'
expect shared-c-stop  "$(line 5 2)" '{"ok": true, "exit": 143}'
gone   shared-b-ended "$b"; gone shared-c-ended "$c"
sleep 1.1
# a game that does not end when asked is killed after a few seconds
conn 3 "$(rq deaf.exe 405)"
expect shared-deaf-started "$(line 3 1)" "$STARTED"
sleep 0.5; d=$(cat "$T/pid.405")
say 3 '{"op":"stop"}'
expect shared-deaf-killed "$(line 3 2 100)" '{"ok": true, "exit": 137}'
gone   shared-deaf-ended "$d"
kill -0 "$other" 2>/dev/null && echo "ok   shared: the prefix's other program outlived every Stop" || { echo "FAIL shared: another program of the prefix ended"; fail=1; }
[ ! -s "$T/wineserver.log" ] && echo "ok   shared: no wineserver command at all" || { echo "FAIL shared: wineserver run:"; cat "$T/wineserver.log"; fail=1; }
sleep 1.1
# MetalSharp's intent applies in the shared prefix too
printf 'vkd3d\n' > "$I/406"
conn 3 "$(rq game.exe 406)"
expect shared-intent-exit "$(line 3 2 50)" '{"ok": true, "exit": 0}'
has    shared-intent-backend "$T/launch.406.log" 'env:GRAPHICS_BACKEND=vkd3d'
has    shared-intent-prefix  "$T/launch.406.log" "env:WINEPREFIX=$SP"
sleep 1.1
# a game_prefix that is not there refuses the launch and creates nothing
sed -i '' "s#\"game_prefix\": \"$SP\"#\"game_prefix\": \"$T/no-such-pfx\"#" "$T/bridge.json"
check shared-prefix-missing "$(rq game.exe 400)" 'the game prefix is not usable'
[ ! -e "$T/no-such-pfx" ] && echo "ok   shared: a missing prefix is not created" || { echo "FAIL shared: the prefix was created"; fail=1; }
kill $other 2>/dev/null; rm -f "$T/pid.other"
sleep 1.1

# requests too large or too deep are refused without harm
check big-string      '{"op": "ping", "x": "'"$(head -c 60000 /dev/zero | tr '\0' a)"'"}' '{"ok": true}'
check oversized       '{"op": "ping", "x": "'"$(head -c 70000 /dev/zero | tr '\0' a)"'"}' 'malformed request'
check deep-30         '{"op": "ping", "x": '"$(printf '[%.0s' $(seq 30))$(printf ']%.0s' $(seq 30))"'}' '{"ok": true}'
check deep-40         '{"op": "ping", "x": '"$(printf '[%.0s' $(seq 40))$(printf ']%.0s' $(seq 40))"'}' 'malformed request'
check deep-20000      '{"op": "launch", "args": '"$(printf '[%.0s' $(seq 20000))"'' 'malformed request'
check deep-objects    '{"op": "ping", "x": '"$(printf '{"k":%.0s' $(seq 40))"'1'"$(printf '}%.0s' $(seq 40))"'}' 'malformed request'
kill -0 $pid 2>/dev/null && echo "ok   daemon alive" || { echo "FAIL the daemon is gone"; fail=1; }
check still-up-2      '{"op": "ping"}' '{"ok": true}'
[ $fail = 0 ] && echo "all passed" || { echo "--- daemon log"; cat "$T/log"; }
exit $fail
