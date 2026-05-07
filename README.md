# rtmpserverdemo

Windows 桌面 RTMP 推流 + 内置 RTMP server demo。原生 C++ / VS2022 / CMake，**不引入第三方库**（RTMP 协议、FLV mux、AMF0 全部自己实现）。

## 功能

- DXGI Desktop Duplication 抓桌面（GPU NV12）
- WASAPI loopback 采集系统音频
- Media Foundation H.264 + AAC 编码（硬编 NVENC/QSV/AMF 失败回退软编）
- 自实现 RTMP 1.0 server（监听 1935）+ 推流 client（同进程 loopback）
- 自实现 FLV tag muxer / AMF0 codec / chunk codec / handshake
- Win32 原生 UI：开始/停止、URL 显示、采集 fps、编码 fps、连接数
- 实测端到端延时 **~230ms**（loopback ffplay）；稳态不增长

## 构建

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\Release\RtmpDesktopStreamer.exe
```

可选 `/auto` 自动开播：`RtmpDesktopStreamer.exe /auto`

## 拉流

低延时（推荐）：

```
ffplay -fflags nobuffer -flags low_delay rtmp://<本机IP>:1935/live/desktop
```

VLC / OBS 也可直接拉。跨电脑访问需放行 Windows 防火墙 1935 端口。

## RTMP 已实现功能

- 握手 C0/C1/C2 ↔ S0/S1/S2（简单握手）
- Chunk Type 0/1/2/3、Extended Timestamp、SetChunkSize 协商
- 控制消息：SetChunkSize / Ack / WindowAckSize / SetPeerBandwidth / UserControl(StreamBegin/SetBufferLength/Ping)
- AMF0 编解码：Number / Boolean / String / Object / Null / ECMA Array / Strict Array
- NetConnection：connect / releaseStream / FCPublish / FCUnpublish / createStream
- NetStream：publish（推流端）/ play（拉流端）/ deleteStream / closeStream
- 媒体消息：Audio (AAC) / Video (AVC, length-prefixed NALU) / AMF0 Data (`@setDataFrame onMetaData`)
- FLV tag：AVCDecoderConfigurationRecord / AudioSpecificConfig 自动构造

未实现：RTMPS / RTMPE / RTMPT / FMS 复杂握手 / AMF3 / 鉴权 / 多 stream。

## 低延时设计

- 新 client 连接 → server 立即 `RequestKeyframe` → 编码器下一帧通过 `MFSampleExtension_CleanPoint` 强制出 IDR（~160ms 内）→ client 从这帧起播，**不发历史 GOP**
- 异步 sender 线程 + 1MB 有界发送队列 + 分级丢帧（满了丢 P 帧 → 丢非关键音频 → 触发新 IDR；IDR / sequence header / metadata 永不丢）
- `SO_SNDBUF=64KB` 阻止 Windows TCP auto-tuning 在内核里堆几 MB 隐形 backlog
- `SO_SNDTIMEO=1s` 防止单个慢 client 拖死生产线程

## 项目结构

```
src/
├── main.cpp                  StreamingPipeline 协调
├── UI.{h,cpp}                Win32 UI
├── DesktopCapture.{h,cpp}    DXGI Desktop Duplication
├── AudioCapture.{h,cpp}      WASAPI loopback
├── VideoEncoder.{h,cpp}      MF H.264 MFT
├── AudioEncoder.{h,cpp}      MF AAC MFT
├── RtmpClient.{h,cpp}        推流端
├── RtmpServer.{h,cpp}        监听 1935 + ClientSession + 异步 sender
├── RtmpProtocol.{h,cpp}      handshake / chunk codec / AMF0
├── FlvMuxer.{h,cpp}          FLV tag + AVC/AAC sequence header
├── DebugLog.{h,cpp}          文件日志
└── Common.h                  ComPtr / HR 宏
```
