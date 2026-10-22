#!/usr/bin/env python3
"""Writes the launcher self-test's Steam fixtures beside this script (#93), from scratch, with no Steam install:

    python3 tests/fixtures/steam/make_fixtures.py [--check]

shortcuts.vdf: a binary VDF as Steam writes userdata/<account>/config/shortcuts.vdf, two non-Steam shortcuts with
every field in Steam's order (a map 0x00, a string 0x01 and an int32 0x02, each type byte then the key, NUL-ended;
0x08 ends a map), one with tags, a non-ASCII name and quotes in its launch options. The launcher's reader and
writer (launcher/src/vdf.cpp) must give these bytes back unchanged; this encoder is separate from it on purpose.

shortcuts_added.vdf: the same with the shortcut the self-test adds after them (ADDED), as Steam would number it.

loginusers.vdf: the text VDF of config/loginusers.vdf, three accounts by SteamID64, one MostRecent.

--check: the files on disk are what this script writes (CI's launcher job; the self-test embeds the files).
"""
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent


def cstr(s):
    return s.encode("utf-8") + b"\0"


def string(key, value):
    return b"\x01" + cstr(key) + cstr(value)


def int32(key, value):
    return b"\x02" + cstr(key) + struct.pack("<i", value)


def mapping(key, items):
    return b"\x00" + cstr(key) + b"".join(items) + b"\x08"


def appid(exe, name):
    """The non-Steam shortcut's id: CRC-32 of the quoted Exe and AppName, top bit set (as a signed int32)."""
    return struct.unpack("<i", struct.pack("<I", zlib.crc32((exe + name).encode("utf-8")) | 0x80000000))[0]


def shortcut(index, name, exe, start_dir, icon="", launch="", hidden=0, overlay=1, desktop=1, last=0,
             tags=()):
    # The fields and their order as Steam writes them (a shortcut added in Steam's own "Add a Non-Steam Game").
    return mapping(str(index), [
        int32("appid", appid(exe, name)),
        string("AppName", name),
        string("Exe", exe),
        string("StartDir", start_dir),
        string("icon", icon),
        string("ShortcutPath", ""),
        string("LaunchOptions", launch),
        int32("IsHidden", hidden),
        int32("AllowDesktopConfig", desktop),
        int32("AllowOverlay", overlay),
        int32("OpenVR", 0),
        int32("Devkit", 0),
        string("DevkitGameID", ""),
        int32("DevkitOverrideAppID", 0),
        int32("LastPlayTime", last),
        string("FlatpakAppID", ""),
        string("sortas", ""),
        mapping("tags", [string(str(i), t) for i, t in enumerate(tags)]),
    ])


# The shortcut the self-test adds (launcher/src/selftest_steam.cpp ADDED_*): Steam's defaults, no icon or tags.
ADDED = ("A Game", '"/home/player/Games/a-game.AppImage"', '"/home/player/Games/"')


def shortcuts_vdf(added=False):
    entries = [
        shortcut(0, "An Emulator", '"/opt/emulator/bin/emulator"', '"/opt/emulator/bin/"',
                 icon="/opt/emulator/share/icon.png", launch='--fullscreen "%command%"', last=1760000000,
                 tags=["Emulation", "Favorites"]),
        shortcut(1, "Jeu à essayer", '"/home/player/Games/jeu/start.sh"', '"/home/player/Games/jeu/"',
                 overlay=0, desktop=0),
    ]
    if added:
        entries.append(shortcut(2, *ADDED))
    return b"\x00" + cstr("shortcuts") + b"".join(entries) + b"\x08" + b"\x08"


LOGINUSERS = """"users"
{
\t"76561198000000111"
\t{
\t\t"AccountName"\t\t"first_account"
\t\t"PersonaName"\t\t"First Player"
\t\t"RememberPassword"\t\t"1"
\t\t"WantsOfflineMode"\t\t"0"
\t\t"SkipOfflineModeWarning"\t\t"0"
\t\t"AllowAutoLogin"\t\t"1"
\t\t"MostRecent"\t\t"0"
\t\t"Timestamp"\t\t"1750000000"
\t}
\t"76561198000000222"
\t{
\t\t"AccountName"\t\t"second_account"
\t\t"PersonaName"\t\t"Second \\"Quoted\\" Player"
\t\t"RememberPassword"\t\t"1"
\t\t"MostRecent"\t\t"1"
\t\t"Timestamp"\t\t"1760000000"
\t}
\t"76561198000000333"
\t{
\t\t"AccountName"\t\t"no_userdata"
\t\t"PersonaName"\t\t"Never Played Here"
\t\t"MostRecent"\t\t"0"
\t\t"Timestamp"\t\t"1700000000"
\t}
}
"""


def main():
    files = {"shortcuts.vdf": shortcuts_vdf(), "shortcuts_added.vdf": shortcuts_vdf(added=True),
             "loginusers.vdf": LOGINUSERS.encode("utf-8")}
    if "--check" in sys.argv[1:]:
        bad = [n for n, data in files.items() if (HERE / n).read_bytes() != data]
        print("steam fixtures: " + ("differ: " + ", ".join(bad) if bad else "up to date"))
        return 1 if bad else 0
    for n, data in files.items():
        (HERE / n).write_bytes(data)
        print(f"wrote {HERE / n} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
