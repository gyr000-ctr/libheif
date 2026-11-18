# ISO/IEC 21496-1 Gain Map 支持 — 实现记录

## 自测命令与结果

构建（最小依赖：内置 uncompressed codec + JPEG，关闭所有可选编解码器）：

```
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DENABLE_PLUGIN_LOADING=OFF -DWITH_REDUCED_VISIBILITY=OFF \
  -DWITH_UNCOMPRESSED_CODEC=ON -DWITH_JPEG_DECODER=ON -DWITH_JPEG_ENCODER=ON \
  -DWITH_AOM_DECODER=OFF -DWITH_AOM_ENCODER=OFF -DWITH_DAV1D=OFF \
  -DWITH_LIBDE265=OFF -DWITH_RAV1E=OFF -DWITH_SvtEnc=OFF -DWITH_X265=OFF \
  -DWITH_X264=OFF -DWITH_KVAZAAR=OFF -DWITH_OpenJPEG_DECODER=OFF \
  -DWITH_OpenJPEG_ENCODER=OFF -DWITH_OPENJPH_ENCODER=OFF -DWITH_FFMPEG_DECODER=OFF \
  -DWITH_OpenH264_DECODER=OFF -DWITH_UVG266=OFF -DWITH_VVDEC=OFF -DWITH_VVENC=OFF
cmake --build build -j12
```

规定的自测命令：

```
ctest -R gainmap_metadata
```

结果：**通过，耗时 ~0.01s（远低于 40s 上限）**，8 个 TEST_CASE 全部通过。

完整套件 `ctest`：93 通过 / 27 跳过（跳过均因本环境无对应编解码器）/ 1 失败。
唯一失败的 `region` 测试经 `git stash` 在改动前的基线上复现，原因是该测试需要
AV1 编码器而本环境只有 JPEG 编码器，与本次改动无关。

## 改动概览

### 新增文件
- `libheif/gainmap.h/.cc` — ISO/IEC 21496-1 GainMapMetadata 载荷的数据模型、
  解析与序列化（含版本/通道/零分母校验），以及通道数交叉校验。
- `libheif/image-items/tmap.h/.cc` — `tmap` 派生图 ImageItem：解析两条 `dimg`
  引用（base + gain map）、定位元数据（tmap 属性 → gain map item 的 hdrgm →
  item 数据）、默认解码回退为 SDR base 图。
- `libheif/api/libheif/heif_gain_map.h/.cc` — 公开 C API：
  `heif_image_handle_has_gain_map`、`..._get_gain_map_metadata`、
  `..._decode_gain_map`、`..._decode_hdr_image`（按 Ultra HDR/21496-1 公式
  在 log2 域插值并 exp2 合成 HDR，支持 8/16 位输入、gain map 最近邻缩放）。
- `tests/gainmap_metadata.cc` — 8 个测试：单/三通道解析、round-trip、hdrgm
  box 工厂、版本不支持、通道数不符、截断/保留位、零分母、子码互异。

### 修改文件
- `box.h/.cc` — 新增 `Box_hdrgm`（ipco 内的 GainMapMetadata 普通 Box），
  并在 `Box::read` 工厂注册 `hdrgm` fourcc。
- `heif_error.h` / `error.cc` — 三个可区分子码：
  `heif_suberror_Gain_map_missing`(153)、
  `heif_suberror_Invalid_gain_map_metadata`(154)、
  `heif_suberror_Invalid_gain_map_channel_count`(155)；
  版本不支持复用 `heif_suberror_Unsupported_data_version`(3002)。
- `mini.cc/.h` —
  - 读路径 `create_expanded_boxes()`：从 mini 紧凑字段生成 gain map item(id 4,
    隐藏 aux，auxC URN `urn:iso:std:iso:ts:21496:-1`)、tmap 派生 item(id 3)、
    各自的 ipco 属性（codec/ispe/pixi/colr/icc/auxC/hdrgm + tmap HDR 元数据）、
    ipma 关联、iloc extent（tmap 内联数据用 construction_method=1/idat）、
    两条 `dimg` + 一条 `auxl` iref。
  - 写路径 `create_from_heif_file()`：识别 tmap item 并回填 mini 的 gain map
    字段（尺寸/位深/色度/CICP/ICC/codec config/数据/元数据/tmap HDR box）；
    `can_convert_to_mini()` 放行 tmap 派生图。
- `context.h/.cc` — `tmap` 加入图片类型白名单与 dimg DAG 遍历；新增
  `get_tmap_image_for_base()` 让主图 handle 也能找到关联 tmap。
- `image_item.cc` — 工厂注册 `tmap`。
- `heif.h` / CMakeLists — 聚合新公开头、登记新源文件与安装头。

## 三类可区分错误
| 条件 | 主码 | 子码 |
|---|---|---|
| hdrgm box / gain map item 缺失 | Invalid_input(2) | Gain_map_missing(153) |
| 通道数与 gain map 分量不符（非 1/3 或不匹配） | Invalid_input(2) | Invalid_gain_map_channel_count(155) |
| 元数据版本不支持 | Unsupported_feature(4) | Unsupported_data_version(3002) |
| 元数据结构损坏/截断/保留位/零分母 | Invalid_input(2) | Invalid_gain_map_metadata(154) / End_of_data(100) |

## SDR 行为不变
`tmap` 默认 `decode_compressed_image()` 委托回 base(SDR) 图；无 gain map 时
`heif_image_handle_decode_hdr_image()` 退化为普通解码。现有 mini/box 等测试
（mini_box、box_equals、mini_security_limits 等）全部通过。
