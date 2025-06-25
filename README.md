# dumrum

Simple stupid ascii character only run-launcher.

No frills, clipboard, movable cursor, nothing special. Makes a bitmap font and a small X window.
(Wayland works just the same but I don't daily drive it and am too lazy to make a wm.)

Only allows valid input at a hard-capped length. Ignores invalid programs, paths and file names.

Space and tab will autocomplete. Enter will attempt to run if the input text is a valid program.

Space after a valid command will allow for typing unverified arguments.

Ctrl+\<c/d/l\> will clear all. Ctrl+backspace will clear to last space or all.

**Note**: some systems dupe /usr/bin, /bin, /usr/sbin and /sbin in PATH.
I edited my OS to reduce the dupes as it's essentially all in /bin (symlink to /usr/bin).
This should only matter if you have long PATH directories or a bunch of dupes.

# usage

```
-h : this message
-d : request server side decorations
-c : center position (not multi-monitor aware)
-k : keep window alive if it loses focus
-b : background color in 0xAARRGGBB hex value (uses stroul, default: 0xFF000000 black)
-f : text color in 0xAARRGGBB hex value (alpha-rgb, default: 0xFFFFFFFF white)
-z : font point size (default: 16)
-F : font name (searches directories in /usr/share/fonts)
-H : window height (default/minimum is set by font size. rest is padding)
-W : window width (default: ridiculous)
```

example: `dumrun -W 250 -H 25 -k -f 0xFF00FF00 -d -c -F RobotoMono-Regular`

# build

`make`

sudo copy to directory for install
