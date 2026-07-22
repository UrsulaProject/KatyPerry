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

Convert one JBT between decrypted/plaintext members and the official BF format:

```sh
./build/BemaniTools jbt-decrypt input.jbt plaintext.jbt
./build/BemaniTools jbt-encrypt plaintext.jbt encrypted.jbt
```

JBHot input additionally needs `--jbhot-plist=/path/to/defaults.plist` on
`jbt-decrypt`. To inspect or rebuild members directly, use `jbt-unpack` and
`jbt-pack`. Recursive directory variants preserve relative paths:

```sh
./build/BemaniTools jbt-unpack-dir /path/to/jbts /path/to/expanded
./build/BemaniTools jbt-pack-dir /path/to/expanded /path/to/repacked
```

Each expanded JBT is represented by a directory named after the original file
stem. `jbt-pack` and `jbt-pack-dir` encrypt output by default; pass
`--encrypt-jbt=false` for plaintext JBTs.

Load JBHot packs. The Loader decrypts both `musicData` and `serverData` directly
from the NSUserDefaults plist:

```sh
./build/BemaniTools load \
  --jbhot /path/to/jbhot-packs \
  --jbhot-plist=/path/to/jp.konmai.jbhot2.T2SSHXUSCG.plist
```

Custom DLC directories accept either BF-encrypted or plaintext JBTs:

```sh
./build/BemaniTools load \
  --official /path/to/official-packs \
  --jbhot /path/to/jbhot-packs \
  --jbhot-plist=/path/to/jbhot-defaults.plist \
  --custom-dir=/path/to/custom-one \
  --custom-dir=/path/to/custom-two \
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

An Official or Custom DLC companion `playlists.plist` is merged with the playlists
from JBHot `serverData`. ID mappings are also applied to playlist entries from the
same DLC. Use
`--playlist-export /path/to/playlists.plist` to write the merged official
`LIST`/`NAME`/`PLID` format without exporting JBTs. Existing 32-character hex
PLIDs are preserved; a random PLID is generated for source playlists without one.

Each DLC is resolved as it is loaded. If an incoming file conflicts with a
different pack already loaded, its directory must contain a `mapping.json` object:

```json
{
  "30000123": 600000000,
  "30000124": 600000001
}
```

Keys are IDs from the JBT filenames and values are the final IDs. Mappings are
always applied, including to JBT `info`, base/ext relationships, and playlists, so
the result is stable when a DLC is loaded by itself. Stale keys, duplicate targets,
identity mappings, filename/info mismatches, missing mappings, and occupied mapping
targets are rejected. Related base/ext packs are merged atomically. Decrypted JBT
members are compared byte-for-byte; a later identical relationship component is
dropped, so CLI priority is Official, JBHot, then Custom command-line order. No IDs
are generated automatically.

The library API is declared in `include/Bemani/JBT.h`. `LoadResult::packs` is a
`std::map<uint32_t, std::vector<MusicPack>>`; lazy loading is the default and eager loading is available through `LoadOptions`.
