# BemaniTools 使用说明

BemaniTools 用来读取、转换和合并 jubeat 的 JBT 曲包、REFLEC BEAT 的 RB 曲包、
mulist、playlists 和 Marker。核心库会自动识别明文、官方 BFCodec、JBHot 和
RBHot 格式；CLI 只负责参数解析。

## 编译

项目提供 `vcpkg.json`，推荐使用 vcpkg manifest 安装 Boost.Program_options、
libzip、libplist 2.x、OpenSSL/libcrypto 和 nlohmann-json。首先准备 vcpkg，然后把
toolchain 传给 CMake：

```sh
git clone https://github.com/microsoft/vcpkg.git /path/to/vcpkg
git -C /path/to/vcpkg checkout 388141d50c018f9050bd0128152f7f8ac7f248b8
/path/to/vcpkg/bootstrap-vcpkg.sh -disableMetrics

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Windows 下将 bootstrap 命令换成 `bootstrap-vcpkg.bat`。vcpkg 会在 CMake
配置时根据 manifest 自动安装锁定的依赖。

如果系统已安装上述依赖，仍可以使用原来的 pkg-config 路径，
无需 vcpkg：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

可以把 CLI 和本文档安装到独立目录：

```sh
cmake --install build --prefix /path/to/package
```

下文假设当前目录是 BemaniTools 仓库，程序路径为：

```text
./build/BemaniTools
```

### GitHub Actions

`Build` workflow 会在 push、pull request 和手动触发时使用锁定的
vcpkg 依赖构建 Linux x64、macOS arm64、macOS x64 和 Windows x64，运行全部
CTest，并上传以下 artifact：

- `BemaniTools-linux-x64`
- `BemaniTools-macos-arm64`
- `BemaniTools-macos-x64`
- `BemaniTools-windows-x64`

每个 artifact 中是一个 `.tar.gz` 或 `.zip` 安装包，CLI 位于包内
`bin/` 目录。这些是普通 workflow 产物，不会自动创建 GitHub Release。

任意路径含空格或中文时都应使用引号。程序不会清空已有输出目录；正式导出时建议
使用空目录，以免旧文件残留。

## 通用格式规则

- JBT、RB、Marker、mulist 和 nolist 高层命令会透明处理各自格式中的随机前缀。
  `.rb` ZIP 成员本身没有四字节前缀；REFLEC BEAT 的运行时 mulist/nolist 有。
  通用 `bfcodec` 命令只处理 BFContainer 本身，不添加或删除 payload 字节。
- `mulist --key` 和 `dlc build --mulist-key` 接收 MD5 之前的原始字符串，
  例如 `SHARED_KEY`。
- DLC 读取顺序固定为 Official → JBHot → Custom 参数顺序。内容完全相同的
  后加载实例会被丢弃。
- 同 ID 内容不同时，不会随机生成新 ID。发生冲突的新 DLC 必须在自己的根目录
  提供 `mapping.json`：

```json
{
  "30000123": 600000000,
  "30000124": 600000001
}
```

JBT 映射的 key 是源 `.jbt` 文件名中的数字 ID，value 是最终 ID。映射同时作用于
JBT 内部 `info.ID`、base/ext 关系、mulist 和 playlists。Marker 的映射则同时修改
`mkXXXX.zip`、banner 名称和生成的 marker list。

RB 映射的 key 同样是源 `.rb` 文件名中的无前导零数字 ID，value 必须是
`100000000`–`2147483647`。映射会同步修改 `.rb` 的 `info.ID`、mulist、nolist 的
`ID/ExtID`、RBHot `mainId` 和 playlist `LIST`；nolist 的商店产品 `PackID`
不会被修改。

## 命令说明

运行总帮助：

```sh
./build/BemaniTools --help
```

运行某个命令的帮助：

```sh
./build/BemaniTools dlc build --help
```

### `bfcodec decrypt|encrypt`

直接解密或生成通用 BFContainer。`--key` 接受 MD5 派生之前的原始字符串。
该命令不会解析明文格式，也不会添加或删除四字节随机前缀。

```sh
./build/BemaniTools bfcodec decrypt \
  --input encrypted.bin \
  --output plaintext.bin \
  --key RAW_KEY

./build/BemaniTools bfcodec encrypt \
  --input plaintext.bin \
  --output encrypted.bin \
  --key RAW_KEY
```

### `dlc build`

统一加载并合并完整 DLC，输出 JBT、`mulist.plist`、可选的加密 `mulist`，以及
存在 playlist 时的 `playlists.plist`。

```sh
./build/BemaniTools dlc build [源目录参数] --output <输出目录> [选项]
```

源目录参数：

- `--official <目录>`：官方 JBT 目录，只能指定一次。
- `--jbhot <目录>`：JBHot JBT 目录，只能指定一次；必须同时传
  `--jbhot-plist <defaults.plist>`。
- `--custom-dir <目录>`：自定义 DLC，可重复传入。目录里的 JBT 可以是明文或
  官方加密格式。
- `--catalog <mulist.plist>`：显式指定官方明文 catalog。未指定时，每个非 Hot
  源会自动读取自己根目录里的 `mulist.plist`。

每个 DLC 目录只扫描根目录中的 `*.jbt`。可选伴随文件为 `mulist.plist`、
`playlists.plist` 和 `mapping.json`。

其他选项：

- `--encrypt-jbt=true|false`：是否将输出 JBT 加密成官方格式，默认 `true`。
- `--mulist-key <原始密钥>`：除明文 `mulist.plist` 外，再输出可直接部署的
  加密 `mulist`。
- `--separate-output`：JBT 分别输出到 `official/`、`jbhot/`、`custom-N/`；
  mulist 和 playlists 仍在输出根目录。
- `--eager`：加载时立刻解密所有资源；默认按需加载。
- `--strict`：遇到第一个无效包立即停止；默认记录错误后继续处理其他包。

默认输出结构：

```text
OUT/
├── 000000001.jbt
├── 000000002.jbt
├── ...
├── mulist.plist        # 始终生成，明文 XML
├── mulist              # 仅指定 --mulist-key 时生成，官方加密格式
└── playlists.plist     # 输入中存在 playlist 时生成
```

ext 关系只来自 catalog/JBHot defaults，不从 JBT 的 `info*` 猜测。如果 base
引用的 ext JBT 不存在，导出会给出警告，并从 mulist 中省略这条 ext 关系。

### `mulist decrypt`

解密原版运行时 `mulist`，自动删除四字节随机前缀，输出明文 XML plist。

```sh
./build/BemaniTools mulist decrypt \
  --input /path/to/mulist \
  --output /path/to/mulist-dec.plist \
  --key SHARED_KEY
```

### `mulist encrypt`

验证输入是 plist array，自动添加四字节随机前缀，再生成官方 BFContainer。

```sh
./build/BemaniTools mulist encrypt \
  --input /path/to/mulist.plist \
  --output /path/to/mulist \
  --key SHARED_KEY
```

### `jbt decrypt`

将单个 JBT 转成成员未加密的 JBT ZIP，但不展开成员文件。官方包和明文包可直接
读取；JBHot 包必须传 defaults plist。

```sh
./build/BemaniTools jbt decrypt \
  --input song.jbt \
  --output song-plain.jbt

./build/BemaniTools jbt decrypt \
  --input hot-song.jbt \
  --output hot-song-plain.jbt \
  --jbhot-plist /path/to/jbhot-defaults.plist
```

### `jbt encrypt`

将明文 JBT 的成员重新打成官方 BFCodec 格式。输出仍是一个 `.jbt` 文件。

```sh
./build/BemaniTools jbt encrypt \
  --input song-plain.jbt \
  --output song-official.jbt
```

JBHot JBT 应先使用带 `--jbhot-plist` 的 `jbt decrypt` 转成明文，再执行
`jbt encrypt`。

### `jbt unpack`

解密并展开单个 JBT。输出目录包含真正的 `info`/`infov2`/`infov3` plist、PNG、
JBSQ、M4A 等资源。

```sh
./build/BemaniTools jbt unpack \
  --input song.jbt \
  --output work/song
```

JBHot 输入同样需要 `--jbhot-plist`。

### `jbt pack`

将一个展开目录重新打成 JBT。默认生成官方加密成员；传 `--plain` 才生成明文成员。
工具保留目录中已有的 `info` 版本。

```sh
./build/BemaniTools jbt pack \
  --input work/song \
  --output song.jbt

./build/BemaniTools jbt pack \
  --input work/song \
  --output song-plain.jbt \
  --plain
```

### `jbt unpack-dir`

递归查找输入目录中的 `.jbt`，批量解密并展开，同时保留相对目录结构。

```sh
./build/BemaniTools jbt unpack-dir \
  --input /path/to/jbt-root \
  --output /path/to/expanded-root \
  [--jbhot-plist /path/to/jbhot-defaults.plist]
```

### `jbt pack-dir`

递归查找包含 `info`、`infov2` 或 `infov3` 的展开目录，批量生成 JBT。默认官方
加密；`--plain` 生成明文 JBT。

```sh
./build/BemaniTools jbt pack-dir \
  --input /path/to/expanded-root \
  --output /path/to/jbt-root
```

### `jbhot defaults-dump`

解密 JBHot defaults plist 中存在的 `musicData`、`serverData`、`userData`、
`offlineData`、`scoreData`，并合并 `musicDetail1...N`。每类数据分别输出格式化
JSON。

```sh
./build/BemaniTools jbhot defaults-dump \
  --input /path/to/jbhot-defaults.plist \
  --output-dir /path/to/json-output
```

## REFLEC BEAT `.rb`

### `rb decrypt|encrypt`

`rb decrypt` 自动识别明文、官方 BF DecodeType 0/1 和 `=RBHOT=` 成员，输出成员为
明文的 `.rb` ZIP。RBHot 输入必须提供对应 defaults plist：

```sh
./build/BemaniTools rb decrypt \
  --input /path/to/100000109.rb \
  --output /path/to/100000109-plain.rb

./build/BemaniTools rb decrypt \
  --input /path/to/hot/806202001.rb \
  --output /path/to/806202001-plain.rb \
  --rbhot-plist /path/to/rbhot-defaults.plist
```

`rb encrypt` 只生成官方 BF 格式，不生成 RBHot。`--decode-type` 可为 `0` 或 `1`，
默认 0；`--plain` 则输出明文成员：

```sh
./build/BemaniTools rb encrypt \
  --input /path/to/100000109-plain.rb \
  --output /path/to/100000109.rb \
  --decode-type 1
```

### `rb unpack|pack`

`rb unpack` 解密并展开单个 `.rb`。目录中会得到真正的 `info` plist、PNG、
`RBFF` note 和 M4A/MP4 资源，未知成员和子目录也会保留。

```sh
./build/BemaniTools rb unpack \
  --input /path/to/100000109.rb \
  --output /path/to/work/100000109
```

`rb pack` 将展开目录重新打包；默认用 DecodeType 0 官方加密，`--decode-type 1`
改用 Type 1，`--plain` 生成明文成员：

```sh
./build/BemaniTools rb pack \
  --input /path/to/work/100000109 \
  --output /path/to/100000109.rb \
  --decode-type 0
```

`rb unpack-dir` 递归查找 `.rb` 并保留相对目录展开；`rb pack-dir` 递归查找含
`info` 的展开目录并生成 `.rb`。参数与单包命令相同：

```sh
./build/BemaniTools rb unpack-dir \
  --input /path/to/rb-root \
  --output /path/to/expanded-root \
  --rbhot-plist /path/to/rbhot-defaults.plist

./build/BemaniTools rb pack-dir \
  --input /path/to/expanded-root \
  --output /path/to/rb-root \
  --decode-type 0
```

### `.rb` 的 MusicData 资源模型

`info` 是 plist dictionary。库显式读取原版 `MusicData` 使用的
`ID/MusicName/MusicNameHira/MusicNameRoman/ArtistName/ArtistNameHira/
ArtistNameRoman/Basic/Medium/Hard/BpmMin/BpmMax`，并保留 `Version`、
`Options` 和所有未知字段的原始 plist。标准 ZIP 成员如下：

| 资源 | 通用/常规成员 | 难度或 Light 成员 | 原版回退 |
| --- | --- | --- | --- |
| 完整音频 | `bgm` | `bgm_b`、`bgm_m`、`bgm_h` | `bgm` |
| 试听音频 | `pre` | 无 | 无 |
| 谱面 | `note_bas`、`note_med`、`note_har` | `note_bas2`、`note_med2`、`note_har2` | 对应的常规谱面 |
| 封面 | `artwork`、`artwork2x` | 在成员末尾加 `_b/_m/_h` | 无 |
| 黑/白曲名 | `title_b`、`title_w`、`title_b2x`、`title_w2x` | 在成员末尾加 `_b/_m/_h` | 无 |
| 黑/白作者 | `artist_b`、`artist_w`、`artist_b2x`、`artist_w2x` | 在成员末尾加 `_b/_m/_h` | 无 |

表中的每个难度/Light 成员都由 `info.Options` 的同名 key 启用。原版只检查 key
是否存在，不检查 value；key 存在但成员读取失败时，仅音频和 Light 谱面会使用表中
的回退成员。Brown 曲名/作者图是原版运行时由 White 图着色生成的，不是额外 ZIP
成员。SPECIAL 谱面来自 nolist 指向的独立 ext `.rb`，原版将 ext 包的
`note_bas`/`note_bas2` 当作主曲的 Special/Light Special。

公共 API 的 `SelectRBAudioResource`、`SelectRBPreviewResource`、
`SelectRBNoteResource`、`SelectRBImageResource` 和 `ResolveRBResource`
复刻上述选择规则。未列出的 ZIP 成员仍保留在 `RBMusicPack.resources`，并参与
去重、解包和重打包。

### `rb build`

统一加载并合并 REFLEC BEAT DLC：

```sh
./build/BemaniTools rb build \
  --official /path/to/official-rb \
  --rbhot /path/to/rbhot-rb \
  --rbhot-plist /path/to/rbhot-defaults.plist \
  --custom-dir /path/to/custom-rb-one \
  --custom-dir /path/to/custom-rb-two \
  --output /path/to/RB-OUT \
  --mulist-key APPLICATION_UNIQUE_ID
```

每个源目录只扫描根目录 `*.rb`，并自动读取：

- `mulist.plist`，或使用 `--mulist-key` 解密运行时 `mulist`；
- `nolist.plist`，或使用同一 key 解密运行时 `nolist`；
- 明文 `playlist.plist` 或运行时 `playlist`；
- 发生 ID 冲突时的 `mapping.json`。

输入优先级固定为 Official → RBHot → Custom 参数顺序。去重 hash 包含全部解密后
的非 `info` 成员名、长度和内容；同最终 ID 且内容一致时静默丢弃后加载实例，
内容不同时要求新 DLC 提供 mapping。

其他选项：

- `--encrypt-rb=true|false`：默认 `true`。
- `--output-key=preserve|0|1`：默认 `preserve`。官方输入保留原 DecodeType；
  明文和 RBHot 输入默认转成 Type 0。
- `--separate-output`：曲包写入 `official/`、`rbhot/`、`custom-N/` 子目录，
  列表仍在输出根目录。
- `--eager`：加载时立即解密所有资源。
- `--strict`：包错误或缺失 base/ext 时立即失败。

默认输出：

```text
RB-OUT/
├── 100000109.rb
├── 806202000.rb
├── 806202001.rb
├── mulist.plist
├── nolist.plist
├── playlist.plist       # 有 playlist 时
├── mulist               # 指定 --mulist-key 时
├── nolist               # 指定 --mulist-key 时
└── playlist             # 指定 key 且有 playlist 时，原版明文运行时格式
```

mulist 只包含普通/base 曲目。官方/custom 的 SPECIAL 关系来自 nolist；RBHot
`mainId` 只用于识别 base/ext 和冲突组件。没有来源 nolist `PackID` 时，工具会
保留 ext `.rb`、从 mulist 排除它、警告并省略该 nolist 项，不会猜测 PackID。

### `rbhot defaults-dump`

解密 RBHot defaults 中存在的 `musicData`、`serverData`、`userData`、
`offlineData`、`scoreData`，并按字段合并 `musicDetail`/`musicDetail1...N`：

```sh
./build/BemaniTools rbhot defaults-dump \
  --input /path/to/rbhot-defaults.plist \
  --output-dir /path/to/rbhot-json
```

### `nolist decrypt|encrypt`

REFLEC BEAT 的 mulist 和 nolist 使用同一 ApplicationUniqueID。nolist 命令会透明
删除或添加四字节随机前缀，`--key` 接受 MD5 之前的原始字符串：

```sh
./build/BemaniTools nolist decrypt \
  --input /path/to/nolist \
  --output /path/to/nolist.plist \
  --key APPLICATION_UNIQUE_ID

./build/BemaniTools nolist encrypt \
  --input /path/to/nolist.plist \
  --output /path/to/nolist \
  --key APPLICATION_UNIQUE_ID
```

### `marker decrypt`

将单个 Marker ZIP 转成成员为明文 PNG 的 ZIP，不展开文件。输入会自动识别官方
BFCodec、JBHot `=JBHOT=` 和已经明文的成员。

```sh
./build/BemaniTools marker decrypt \
  --input mk0048.zip \
  --output mk0048-plain.zip
```

### `marker encrypt`

读取明文、官方或 JBHot Marker ZIP，统一输出官方 BFCodec Marker ZIP。四字节
前缀和 ZIP 尾部 MD5 都由工具自动生成。

```sh
./build/BemaniTools marker encrypt \
  --input mk0048-plain.zip \
  --output mk0048.zip
```

### `marker unpack`

解密并展开单个 Marker ZIP，输出真正以 PNG 签名开头的帧文件。

```sh
./build/BemaniTools marker unpack \
  --input mk0048.zip \
  --output work/mk0048
```

### `marker pack`

将展开的 PNG 帧目录打成官方加密 Marker ZIP。它已经包含 `marker encrypt` 的
工作，不需要再加密第二次。

```sh
./build/BemaniTools marker pack \
  --input work/mk0048 \
  --output mk0048.zip
```

原版动画要求 88 个帧文件：`ma00`–`ma23`，以及 `h100`–`h115`、
`h200`–`h215`、`h300`–`h315`、`h400`–`h415`。这些文件本身是 PNG，文件名
没有 `.png` 后缀。

### `marker unpack-dir`

递归查找输入目录中的 Marker ZIP 并批量展开，保留相对目录结构。

```sh
./build/BemaniTools marker unpack-dir \
  --input /path/to/marker-root \
  --output /path/to/expanded-root
```

### `marker pack-dir`

递归查找名为 `mk<数字>` 的展开目录并批量生成对应的官方加密 `.zip`。

```sh
./build/BemaniTools marker pack-dir \
  --input /path/to/expanded-root \
  --output /path/to/marker-root
```

### `marker build`

合并完整 Marker 目录。每个输入目录只扫描根目录中实际存在的 `mk<数字>.zip`，
Marker ID 不要求连续。

```sh
./build/BemaniTools marker build \
  [--official /path/to/official-marker] \
  [--jbhot /path/to/hot-marker] \
  [--custom-dir /path/to/custom-marker] \
  --output /path/to/output-marker \
  --marker-list-output /path/to/output-marker/marker-list.plist
```

输入目录布局：

```text
CustomMarker/
├── mk0048.zip
├── mk0123.zip
├── mapping.json                    # 仅发生 ID 冲突时需要
└── banner/
    ├── tm0048_banner.png
    └── tm0123_banner.png
```

banner 是独立的 128×80 PNG，不放进 Marker ZIP。建议使用原版兼容的四位 ID
`0000`–`9999`。缺 banner 时，默认仍保留 Marker ZIP 和列表项，但不生成不存在的
banner，并以非零状态报告；`--strict` 会立即停止。

输出始终统一为官方 BFCodec Marker ZIP。`--marker-list-output` 生成的是明文 XML
plist，应直接放在最终 Marker 目录中。Tweak 读取该文件并调用游戏自己的
`+[MarkerManager setMarkerList:]`；BemaniTools 不再生成加密的
`PrefMarkerInfoList`。

### `marker-list decrypt`

只用于检查原版已有的 `PrefMarkerInfoList`。输入可以是 raw NSData 文件或 Base64
文本，输出普通 XML plist。该命令没有对应的 encrypt 命令。

```sh
./build/BemaniTools marker-list decrypt \
  --input /path/to/PrefMarkerInfoList \
  --output /path/to/marker-list.plist
```

## 傻瓜流程一：合并官方和 JBHot 曲包到 OUT

将下面的占位路径替换成自己的输入和输出目录。这一条命令完成：

1. 加载官方 JBT 和官方目录内的 `mulist.plist`。
2. 通过 JBHot defaults 解密并加载 Hot JBT、catalog 和 playlists。
3. 按 Official → Hot 的优先级处理重复和 `mapping.json`。
4. 输出官方加密 JBT、明文 `mulist.plist`、密钥为 `SHARED_KEY` 的加密 `mulist`，
   以及存在时的 `playlists.plist`。

```sh
./build/BemaniTools dlc build \
  --official "/path/to/official-jbt" \
  --jbhot "/path/to/jbhot-jbt" \
  --jbhot-plist "/path/to/jbhot-defaults.plist" \
  --output "/path/to/OUT" \
  --encrypt-jbt=true \
  --mulist-key SHARED_KEY
```

如果官方 `mulist.plist` 不在官方 JBT 目录里，额外添加：

```text
--catalog "/path/to/official-mulist.plist"
```

要再加入自己的 JBT DLC，在同一命令末尾重复添加：

```text
--custom-dir "/path/to/custom-dlc-one" \
--custom-dir "/path/to/custom-dlc-two"
```

每个 Custom 目录放自己的 JBT、可选 `mulist.plist`、`playlists.plist` 和发生冲突
时的 `mapping.json`。

## 傻瓜流程二：修改或添加 Marker

解包一个 Marker：

```sh
./build/BemaniTools marker unpack \
  --input "/path/to/mk0048.zip" \
  --output "/path/to/MarkerWork/mk1234"
```

这里把参考 Marker 的帧放进新的空闲 ID `1234`。修改帧后，直接打回官方加密格式：

```sh
./build/BemaniTools marker pack \
  --input "/path/to/MarkerWork/mk1234" \
  --output "/path/to/MyMarkers/mk1234.zip"
```

如果只是原地替换已有的 `mk0048.zip`，可以把 `marker pack` 的输出直接写成新的
`mk0048.zip`，原 Preferences 中已有的 Marker 列表不需要改变。只有添加新 ID
时才需要下面的 `marker build` 和新 `marker-list.plist`。

将 128×80 banner 放到：

```text
/path/to/MyMarkers/banner/tm1234_banner.png
```

最后把 Hot Marker 和自己的 Marker 合并到 OUT。生成的明文 plist 就放在 Marker
目录根部：

```sh
./build/BemaniTools marker build \
  --jbhot "/path/to/jbhot-marker" \
  --custom-dir "/path/to/MyMarkers" \
  --output "/path/to/OUT/marker" \
  --marker-list-output "/path/to/OUT/marker/marker-list.plist"
```

最终目录应类似：

```text
OUT/marker/
├── mk0001.zip
├── mk0048.zip
├── mk1234.zip
├── mk9999.zip
├── marker-list.plist
└── banner/
    ├── tm0001_banner.png
    ├── tm0048_banner.png
    ├── tm1234_banner.png
    └── tm9999_banner.png
```

将整个目录部署到游戏的 Marker 目录即可。Tweak 会读取同目录的
`marker-list.plist`，再交给原版 `MarkerManager` 保存到 Preferences。
