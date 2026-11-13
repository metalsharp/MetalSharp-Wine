#!/bin/bash
# Starts the guest desktop (Xvnc :1 + openbox) and the native arm64 Steam client with a CPU-rendered UI.
# Steam's children inherit SYSTEM_PATH as their PATH, so /usr/local/bin (our xdg-open shim) must lead it.
export PATH=/usr/local/bin:/usr/bin:/bin
export DISPLAY=:1 XDG_RUNTIME_DIR=/run/user/$(id -u)
S=$HOME/.local/share/Steam
pgrep -x Xvnc >/dev/null || { nohup Xvnc :1 -geometry 5120x2880 -depth 24 -rfbauth $HOME/.vnc/passwd -localhost no -AlwaysShared >>$HOME/xvnc.log 2>&1 & sleep 2; }
# The Mac shows every Steam window in a Mac window of its own (metalsharp-steamview), each one a crop of this
# one big screen: mtsp-windows lays the windows out on it side by side and reports their rectangles. So
# Linux window frames are left off, nothing is maximized, and openbox gets no key bindings and no mouse
# bindings at all, because nothing but mtsp-windows may move a window. Not even click-to-focus: openbox
# does that by grabbing the press, and to Steam a grab looks like its window losing focus, so every open
# drop-down list closed just before the click reached it. The Mac window that becomes key asks
# mtsp-windows to focus its guest window instead.
mkdir -p $HOME/.config/openbox
cat > $HOME/.config/openbox/rc.xml <<'RC'
<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <focus><focusNew>yes</focusNew><followMouse>no</followMouse></focus>
  <placement><policy>Smart</policy><center>yes</center></placement>
  <desktops><number>1</number></desktops>
  <applications>
    <application class="*"><decor>no</decor></application>
  </applications>
  <keyboard></keyboard>
  <mouse><dragThreshold>8</dragThreshold></mouse>
</openbox_config>
RC
xsetroot -solid '#171d25' 2>/dev/null
if pgrep -x openbox >/dev/null; then openbox --reconfigure; else nohup dbus-launch --exit-with-session openbox >>$HOME/openbox.log 2>&1 & sleep 1; fi
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
pgrep -f "python3 /usr/local/bin/mtsp-filemanager1" >/dev/null || { nohup /usr/local/bin/mtsp-filemanager1 >>$HOME/filemanager1.log 2>&1 & }
# Steam's "Add Drive" opens Wine's folder browser on the Mac (mtsp-steamhook, over Steam's debugging port,
# which this file turns on and which listens inside the virtual machine only)
touch $S/.cef-enable-remote-debugging
pgrep -f "python3 /usr/local/bin/mtsp-steamhook" >/dev/null || { nohup /usr/local/bin/mtsp-steamhook >>$HOME/steamhook.log 2>&1 & }
mkdir -p $HOME/.steam; ln -sfn $S $HOME/.steam/steam; ln -sfn $S $HOME/.steam/root
ln -sfn $S/steamrtarm64 $HOME/.steam/bin64; ln -sfn $S/steamrtarm64 $HOME/.steam/bin
# Steam starts every game through ~/.steam/sdkarm64/steam-launch-wrapper; without this link no
# compatibility tool is ever reached ("steam-launch-wrapper: not found" and the game stops at once)
ln -sfn $S/linuxarm64 $HOME/.steam/sdkarm64
cd $S
# Steam asks to be restarted (exit 42) after an update or "Restart Steam"; keep it alive, and have its
# children find our xdg-open first. Append to the log so re-running this never truncates a running client's.
export SYSTEM_PATH="$PATH" SYSTEM_LD_LIBRARY_PATH="$S/steamrtarm64:$S/steamrtarm64/libs:$S/linuxarm64"
export LD_LIBRARY_PATH="$SYSTEM_LD_LIBRARY_PATH" STEAM_RUNTIME=0
if pgrep -u "$(id -u)" -x steam >/dev/null; then
  echo "steam already running"; exit 0
fi
# mtsp-statfs.so (when metalsharp-steam could build it) has Steam show the disk its games are on
[ -f /usr/local/lib/metalsharp/mtsp-statfs.so ] && export LD_PRELOAD=/usr/local/lib/metalsharp/mtsp-statfs.so
# Before each start of Steam, mtsp-compat sees to the MetalSharp compatibility tools and the default one.
# Here as well as from the Mac, because Steam writes its configuration first when someone has signed in
# and restarts itself after that: the default is then in place without the virtual machine starting again.
# It is run without the variables above, which are for Steam's own programs.
nohup bash -c 'while :; do
  env -u LD_PRELOAD -u LD_LIBRARY_PATH /usr/local/bin/mtsp-compat >>"$HOME/steam.log" 2>&1
  ./steamrtarm64/steam -cef-disable-gpu "$@" >>"$HOME/steam.log" 2>&1; [ $? -eq 42 ] || break; done' steam-loop "$@" >/dev/null 2>&1 &
echo "steam supervisor pid $!"
