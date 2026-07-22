# BemaniTools

C++20 library and CLI for loading plaintext, official BF-encrypted, and JBHot JBT music packs.

## Build

Dependencies: libzip, libplist 2.x, OpenSSL/libcrypto, and json-c.

```sh
/opt/homebrew/bin/cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
/opt/homebrew/bin/cmake --build build -j4
```

## CLI

The CLI uses command groups. Run `BemaniTools --help` or append `--help` to any
command for its complete options.

Decrypt or encrypt a runtime mulist with the raw string used before MD5 key
derivation:

```sh
./build/BemaniTools mulist decrypt \
  --input /path/to/mulist --output /path/to/mulist.plist --key SHARED_KEY
./build/BemaniTools mulist encrypt \
  --input /path/to/mulist.plist --output /path/to/mulist --key SHARED_KEY
```

Convert, expand, and repack JBTs. The `*-dir` forms recurse while preserving
relative paths. JBHot inputs require the encrypted defaults plist. Packing uses
official BF encryption unless `--plain` is supplied.

```sh
./build/BemaniTools jbt unpack --input song.jbt --output expanded/song
./build/BemaniTools jbt pack --input expanded/song --output song.jbt
./build/BemaniTools jbt decrypt --input hot.jbt --output plain.jbt \
  --jbhot-plist /path/to/defaults.plist
```

Merge complete DLC sources and export JBTs, mulist, and playlists:

```sh
./build/BemaniTools dlc build \
  --official /path/to/official \
  --jbhot /path/to/hot \
  --jbhot-plist /path/to/defaults.plist \
  --custom-dir /path/to/custom-one \
  --custom-dir /path/to/custom-two \
  --output /path/to/output \
  --encrypt-jbt=true --separate-output --mulist-key SHARED_KEY
```

Decrypt the known JBHot defaults payloads to formatted JSON:

```sh
./build/BemaniTools jbhot defaults-dump \
  --input /path/to/defaults.plist --output-dir /path/to/json
```

Marker conversion mirrors JBT conversion. Decrypted/expanded members are real
PNG files without the game's four-byte prefix; generated ZIPs use official BF
encryption. `marker build` scans only actual `mk<id>.zip` files and exports the
matching external banners.

```sh
./build/BemaniTools marker unpack --input mk0048.zip --output expanded/mk0048
./build/BemaniTools marker pack --input expanded/mk0048 --output mk0048.zip
./build/BemaniTools marker build \
  --official /path/to/official-marker \
  --jbhot /path/to/hot-marker \
  --custom-dir /path/to/custom-marker \
  --output /path/to/marker-output \
  --marker-list-output /path/to/marker-output/marker-list.plist
```

Marker-list plaintext is an XML plist array containing `markerID`, `bannerName`,
and `version`. `marker build` writes this plaintext form so an injected tweak can
load it and pass the array directly to the game's `+[MarkerManager setMarkerList:]`.
An integration example is provided in `ios/MarkerListLoaderExample.xm`.

The standalone decrypt command remains available for inspecting an existing
encrypted `PrefMarkerInfoList`. BemaniTools deliberately does not rebuild this
archive; installation goes through the game's own `NSKeyedArchiver` and BFCodec
implementation.

```sh
./build/BemaniTools marker-list decrypt \
  --input PrefMarkerInfoList --output marker-list.plist
```

Exported music JBTs and marker ZIP members use official BF encryption. Runtime
formats receive their original four-byte prefix: mulist, infov3, and official marker frames
use random bytes, while decryption normalizes the application-visible payload.

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
the result is stable when a DLC is loaded by itself. Missing mappings, stale keys,
duplicate targets, and occupied mapping targets are rejected. Filename IDs and
`info.ID` are tracked separately because real JBHot archives do not always make
them equal. Related base/ext packs are merged atomically. Decrypted JBT members are
compared byte-for-byte; a later identical relationship component is
dropped, so CLI priority is Official, JBHot, then Custom command-line order. No IDs
are generated automatically.

The music and marker APIs are declared in `include/Bemani/JBT.h` and
`include/Bemani/Marker.h`. `LoadResult::packs` remains a
`std::map<uint32_t, std::vector<MusicPack>>`; music resources are lazy by
default and can be materialized eagerly with `LoadOptions`.
