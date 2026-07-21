# BemaniTools

C++20 library and CLI for loading plaintext, official BF-encrypted, and JBHot JBT music packs.

## Build

Dependencies: libzip, libplist 2.x, OpenSSL/libcrypto, and json-c.

```sh
/opt/homebrew/bin/cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
/opt/homebrew/bin/cmake --build build -j4
```

## CLI

Decrypt the original runtime `mulist` with an `ApplicationUniqueID` Keychain dump:

```sh
./build/BemaniTools mulist-decrypt \
  /path/to/Documents/mulist \
  /path/to/__private_info/_1 \
  /path/to/Documents/mulist-dec.plist
```

Load official packs (a companion `mulist.plist` is discovered automatically):

```sh
./build/BemaniTools load --official /path/to/official-packs
```

Load JBHot packs. The Loader decrypts both `musicData` and `serverData` directly
from the NSUserDefaults plist:

```sh
./build/BemaniTools load \
  --jbhot /path/to/jbhot-packs \
  --jbhot-plist=/path/to/jp.konmai.jbhot2.T2SSHXUSCG.plist
```

Custom DLC directories accept either BF-encrypted or plaintext JBTs. Every Custom
DLC must be followed by its own non-overlapping allocation range:

```sh
./build/BemaniTools load \
  --official /path/to/official-packs \
  --jbhot /path/to/jbhot-packs \
  --jbhot-plist=/path/to/jbhot-defaults.plist \
  --custom-dir=/path/to/custom-one --custom-range=1100,2200 \
  --custom-dir=/path/to/custom-two --custom-range=2211,2300 \
  --resolve 600000000 899999999 \
  --encrypt-jbt=true \
  --separate-output \
  --mulist-key=SHARED_KEY \
  --export /path/to/output
```

Exported JBT members use the official BF encryption by default; pass
`--encrypt-jbt=false` for plaintext members. `mulist.plist` is always written in
plaintext. When `--mulist-key` is present, the exporter additionally writes an
official BF-encrypted `mulist`, deriving the codec key from the supplied raw key
and prepending four random bytes before the plist as the game expects.

With `--separate-output`, JBTs are grouped by input DLC under `official/`,
`jbhot/`, and `custom-1/`, `custom-2/`, ... in Custom command-line order.
The merged `mulist`, `mulist.plist`, and `playlists.plist` remain in the output
root. This organized layout is intended for staging; the game itself expects
the JBT files directly in its `Documents` directory.

An Official DLC companion `playlists.plist` is merged before the playlists from
JBHot `serverData`. Conflict resolution remaps only the JBHot playlist IDs. Use
`--playlist-export /path/to/playlists.plist` to write the merged official
`LIST`/`NAME`/`PLID` format without exporting JBTs. Existing 32-character hex
PLIDs are preserved; a random PLID is generated for source playlists without one.

Official IDs never change. JBHot IDs only move when they conflict with a
non-identical Official pack. A conflicting Custom component uses that Custom DLC's
own range. Before remapping, decrypted JBT members are compared byte-for-byte and
later identical instances are dropped with priority Official, JBHot, then Custom
command-line order. JBHot playlists follow any resulting JBHot ID changes.

The library API is declared in `include/Bemani/JBT.h`. `LoadResult::packs` is a
`std::map<uint32_t, std::vector<MusicPack>>`; lazy loading is the default and eager loading is available through `LoadOptions`.
