# 小智音乐播放器 · Meting 音源 + 实时歌词

基于 [虾哥 xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 与 [wuooo339/xiaozhimusic](https://github.com/wuooo339/xiaozhimusic) 的二次开发固件：在原有语音助手能力之上，接入 **Meting（网易云）公共音源**，实现「说一句话就能放歌」，并在 LCD 底部实时滚动歌词。

> 本仓库为个人改造版，非上游官方版本。默认针对 **ESP32-S3 + ST7789 240×320 + CH343 串口** 的面包板方案调整。

---

## 特性

| 能力 | 说明 |
|---|---|
| 🎵 语音点歌 | 说「放一首周杰伦的晴天」即搜索并播放，关键词自动提取（去掉「放一首」「的歌」等口语） |
| 🔊 320 kbps 音质 | Meting 以 `br=320` 取网易云直链（`type=url` 返回 302 → CDN 真实 MP3 地址） |
| 📜 实时歌词 | 拉取 LRC，按实际播放进度高亮当前行，底部三行滚动（上一行 / 当前行 / 下一行） |
| 🏷️ 歌名浮显 | 每首歌开头显示「歌名 - 歌手」4 秒后自动隐藏，不占歌词空间 |
| 🎙️ 播放中可打断 | 音乐播放时仍可喊「小智」唤醒并下指令（修复了唤醒任务被饿死的问题） |
| 🔁 双解码器 | MP3 走 ESP-ADF `http_stream` + MP3 解码；M4A 走内置解码器，按 URL 自动分流 |
| 🌐 双音源实例 | 搜索与「直链/歌词」分别使用不同 Meting 实例，规避单个实例的搜索限流 |

## 与上游的差异

- **音源**：由 `vkeys` 换为 Meting 公共实例（网易云），不再需要私有密钥。
  - 搜索：`https://met.liiiu.cn/meting/api`（失败自动重试一次 → 退回 `https://api.qijieya.cn/meting/`）
  - 直链 / 歌词：`https://api.qijieya.cn/meting/`
- **音质链路**：解码固定 44.1 kHz → 线性插值重采样到设备输出采样率（24 kHz），跨块保留相位与边界样本，长时间播放不漂移。
- **歌词**：新增 `LyricParser`（多维时间戳、间奏空行、元数据忽略、二分查找）+ 底部叠加面板。
- **唤醒**：`afe_wake_word` 检测任务优先级 3 → 7，避免音乐播放时「喂数据」任务占满 ringbuffer 导致喊不动。
- **稳定性**：HTTP 客户端补 `crt_bundle_attach`，解决 HTTPS 音频流的 `SSL_SETUP_FAILED`；播放队列加高水位限流，杜绝静默丢帧。

## 硬件

默认板型：`CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI_CAM`（面包板 ESP32-S3 + 摄像头排针）。

| 部件 | 型号 / 引脚 |
|---|---|
| 主控 | ESP32-S3（16 MB Flash） |
| 屏幕 | ST7789 SPI 240×320，MOSI 20 / CLK 19 / DC 47 / RST 21 / CS 45 / BL 38 |
| 扬声器 | I2S 功放（MAX98357A 等），DOUT 39 / BCLK 40 / LRCK 41 |
| 麦克风 | I2S 数字麦，WS 1 / SCK 2 / DIN 42 |
| 串口 | CH343 USB 转串口（Windows 下通常为 `COM3`） |

其他板型请在 `idf.py menuconfig → Xiaozhi Assistant → Board Type` 中切换，并在对应 `main/boards/<board>/config.h` 里确认屏幕与采样率定义。

## 快速开始

### 1. 环境

- ESP-IDF **v5.4.4**
- ESP-ADF **v2.7**（`ADF_PATH` 必须指向 ADF 根目录）
- Python 3.11 虚拟环境（IDF 自带）

```bash
# Linux / macOS
. $IDF_PATH/export.sh
export ADF_PATH=/path/to/esp-adf
```

Windows 下 **不要用 Git Bash** 执行 `export.sh`（会报 `MSys/Mingw is not supported`），请用 PowerShell：

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$env:ADF_PATH = "C:\Espressif\esp-adf"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\python_env\idf5.4_py3.11_env"
. C:\Espressif\frameworks\esp-idf-v5.4.4\export.ps1
```

### 2. 配置

```bash
idf.py menuconfig
```

需要确认的项：

- `Xiaozhi Assistant → Board Type`：选择你的板型
- `Component config → LCD`：`ST7789 240x320`（与实物一致）
- Wi-Fi 可在首次开机时通过配网热点（`192.168.4.1`）配置，无需写死在代码里

### 3. 编译

```bash
idf.py build
```

### 4. 烧录

**推荐：只烧 app 分区**（保留 NVS 里的 Wi-Fi 配置与音量设置）

```bash
idf.py -p COM3 app-flash
```

**整片烧录**（首次或分区表变更时；会清空 NVS，需要重新配网）

```bash
# 先合并为单文件（Windows PowerShell 示例）
python -m esptool --chip esp32s3 merge_bin -o build/merged-binary.bin `
  --flash_mode dio --flash_freq 80m --flash_size 16MB `
  0x0 build/bootloader/bootloader.bin `
  0x8000 build/partition_table/partition-table.bin `
  0xd000 build/ota_data_initial.bin `
  0x10000 build/srmodels/srmodels.bin `
  0x100000 build/xiaozhi.bin

python -m esptool --chip esp32s3 -p COM3 -b 460800 write_flash 0x0 build/merged-binary.bin
```

### 5. 看日志

```bash
idf.py -p COM3 monitor
```

歌词链路的诊断日志长这样：

```
Lyrics downloaded: 4213 bytes, head: [00:00.00] 作词 : ...
Lyrics loaded: 晴天 - 周杰伦, 68 lines, aligned at 12480ms -> line 9
ShowLyric line=9 cur=故事的小黄花 next=从出生那年就飘着
Lyric progress: pos=15000ms line=11 total=68
```

## 使用

### 语音

| 说法 | 行为 |
|---|---|
| 「放一首晴天」 | 搜索并播放第一首匹配结果 |
| 「播放周杰伦的歌」 | 按歌手搜索 |
| 「小智，停止播放」 | 停止音乐并恢复待机 |

播放过程中可以直接喊唤醒词打断，助手会停止音乐进入对话。

### MCP 工具

设备端通过 MCP 协议暴露以下工具，云端大模型据此控制设备：

| 工具 | 参数 | 说明 |
|---|---|---|
| `test_search_music` | `keyword` | 搜索并播放（核心入口） |
| `control_music` | `action`（stop / pause） | 控制播放 |
| `self.audio_speaker.set_volume` | `volume` | 音量 |
| `self.get_device_status` | — | 电量、网络、音量等状态 |
| `self.screen.set_brightness` | `brightness` | 屏幕亮度 |
| `self.screen.set_theme` | `theme` | 主题切换 |
| `self.camera.take_photo` | `question` | 拍照识图 |

## 实现说明

### 音频链路

```
Meting(type=url, br=320) ──302──▶ 网易云 CDN (.mp3)
        │
        ├─ MP3：ESP-ADF http_stream ─▶ MP3 decoder ─▶ 44.1kHz PCM
        └─ M4A：M4aDecoder（内置，支持 Range 请求）
                              │
                    线性插值重采样 44.1k → 24k（跨块相位连续）
                              │
                    audio_playback_queue_（上限 64 块，高水位 48 暂停生产）
                              │
                    AudioOutputTask ─▶ I2S DMA（6 × 240 帧 ≈ 60ms）──▶ 喇叭
```

关键点：

- **HTTPS**：`http_stream` 必须设置 `crt_bundle_attach = esp_crt_bundle_attach`，否则普通地址能播、音乐 CDN 的 https 直链会报 `No server verification option set`。
- **重采样**：解码器固定输出 44.1 kHz，设备输出 24 kHz。重采样器保留「上一块最后一个样本」与「跨块相位」，避免块边界爆音与长期累计漂移。
- **缓冲**：播放队列积压时暂停生产而不是丢帧，网络抖动只表现为短暂停顿，不会破音。

### 歌词链路

```
Meting(type=lrc) ──▶ 纯文本 LRC（兼容 JSON 包装 {"lyric":"..."}）
        │
   LyricParser::Parse —— 多维时间戳、忽略 [ar:]/[ti:] 元数据、空行保留为间奏
        │
   播放进度：music_played_samples_ / output_sample_rate - 60ms(DMA)
        │
   行号变化 ─▶ on_lyric_changed(line, cur, prev, next, song, artist)
        │
   Schedule → 主任务 → LcdDisplay::ShowLyric（LVGL 加锁更新）
```

两个容易踩的坑，已在代码里处理：

1. **进度累加不能依赖歌词是否存在**。歌词在起播后 1~3 秒才下载完成，若那段时间不计数，`SetLyrics()` 算出的「已播时长」会偏小，歌词整体滞后一段且永远追不回来。
2. **歌词面板不要用 flex 布局**。固定高度容器里 flex 会把子项压成 0 高度——结果是「黑框出现了但一个字都没有」。现在改为绝对定位 + 固定宽度。

## 可调参数

| 位置 | 参数 | 说明 |
|---|---|---|
| `main/audio/audio_service.cc` | `LYRIC_OUTPUT_LATENCY_MS`（默认 60） | 歌词提前/延后补偿量，**调大 = 歌词更早切**，调小 = 更晚切 |
| `main/mcp_server.cc` | `METING_SEARCH_BASE` / `METING_STREAM_BASE` | 音源地址，公共实例失效时可自行部署 Meting 后替换 |
| `main/boards/bread-compact-wifi-s3cam/config.h` | `AUDIO_OUTPUT_SAMPLE_RATE`（24 kHz） | 不建议改到 48 kHz：silk 重采样器与 DMA 内存都会撑不住 |
| `main/display/lcd_display.cc` | 歌词面板高度 / 配色 | 面板 116（无歌名）/ 138（显示歌名）两套布局 |

## 常见问题

**烧录报 `Could not open COM3, the port is busy`**
串口被占用，多半是之前的 `idf.py monitor` 没退出。关掉监视器窗口，或查杀残留进程：

```powershell
Get-CimInstance Win32_Process -Filter "Name='python.exe'" | Where-Object { $_.CommandLine -match 'idf' }
```

**PowerShell 导入环境报「禁止运行脚本」**

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
```

**编译报 `MSys/Mingw is not supported`**
说明在 Git Bash / MSYS 里执行了 `export.sh`，改用 PowerShell 的 `export.ps1`。

**播不出声 / 只有提示音**
确认 `sdkconfig` 里的板型与实物一致（尤其 `CONFIG_LCD_*` 与 `CONFIG_BOARD_TYPE_*`），板型错了会导致 I2S 引脚全错。

**歌词比人声慢一拍 / 抢拍**
调 `LYRIC_OUTPUT_LATENCY_MS`。若偏差是「几秒」量级，多半不是补偿问题，而是 LRC 与音频版本不匹配（同首歌不同版本前奏长度不同）。

**个别汉字显示成空白或方块**
字库是裁剪过的 `font_puhui_16_4`，生僻字缺字形。要彻底解决需换全字库字体（体积增加 1~2 MB）。

**整片烧录后 Wi-Fi 没了**
整片烧录会覆盖 NVS 分区。要么重新配网（连设备热点 → `192.168.4.1`），要么平时只用 `idf.py app-flash`。

## 目录结构

```
main/
├── audio/
│   ├── audio_service.cc        # 音频任务、播放队列、歌词进度驱动
│   ├── esp_adf_mp3_player.cc   # MP3 走 ESP-ADF http_stream + decoder
│   ├── m4a_decoder.cc          # M4A 解码 + 格式自动识别
│   ├── lyric_parser.cc         # LRC 解析（时间戳 / 元数据 / 二分查找）
│   └── wake_words/afe_wake_word.cc  # 唤醒词，播放时也能唤醒
├── display/
│   └── lcd_display.cc          # 主界面 + 底部歌词面板
├── mcp_server.cc               # MCP 工具：搜索 / 播放 / 音量 / 拍照
├── application.cc              # 主事件循环，歌词回调调度
└── boards/bread-compact-wifi-s3cam/config.h   # 本仓库默认板型
```

## 致谢

- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)：小智 AI 聊天机器人固件底座
- [wuooo339/xiaozhimusic](https://github.com/wuooo339/xiaozhimusic)：本项目 fork 的上游音乐播放实现
- [Meting](https://github.com/metowolf/Meting)：网易云等平台音乐 API 封装
- [ESP-ADF](https://github.com/espressif/esp-adf)：`http_stream` 与 MP3 解码
- 上游演示视频：[ESP32 小智播放 URL 网络音乐](https://www.bilibili.com/video/BV1b3adzNErv/)

`README_en.md` / `README_ja.md` 为上游原版英文与日文文档，音源部分已过时，请以本文件为准。

## 许可证

MIT，详见 [LICENSE](LICENSE)。
