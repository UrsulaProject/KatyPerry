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
./build/BemaniTools load /path/to/official-packs
```

Load JBHot packs:

```sh
./build/BemaniTools load \
  --jbhot-plist /path/to/jp.konmai.jbhot2.T2SSHXUSCG.plist \
  --server-data /path/to/serverData.json \
  /path/to/jbhot-packs
```

Resolve conflicts and export BF-encrypted JBTs plus a plaintext official-format `mulist.plist`:

```sh
./build/BemaniTools load \
  --jbhot-plist /path/to/jbhot-defaults.plist \
  --server-data /path/to/serverData.json \
  --resolve 600000000 899999999 \
  --export /path/to/output \
  /path/to/official-packs /path/to/jbhot-packs
```

When `--server-data` is present, JBHot playlists are exported as an official-format
`playlists.plist`. Playlist music IDs follow the JBHot pack instance when conflict
resolution assigns that instance a new ID.

The library API is declared in `include/Bemani/JBT.h`. `LoadPacks` returns
`std::map<uint32_t, std::vector<MusicPack>>`; lazy loading is the default and eager loading is available through `LoadOptions`.
