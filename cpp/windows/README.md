# Windows resources

`warlords2.rc` and `warlords2.manifest` are what a Windows build of the game
links in: the icon and version, and a manifest that makes the narrow file
calls take UTF-8 (so a user folder named `C:\Users\Åsa` holds saves as well
as any other).

The Bazel build (`cpp/BUILD.bazel`) does not target Windows yet. When it
does, the game's `cc_binary` needs these two files, the `-mwindows` (or
`/SUBSYSTEM:WINDOWS`) link option, and `SDL_MAIN_HANDLED`.
