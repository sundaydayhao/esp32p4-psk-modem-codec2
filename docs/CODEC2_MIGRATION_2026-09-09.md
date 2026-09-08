# Codec2 与鲁棒 PSK 第一阶段

本次范围是 **PSK125R、PSK250R、部分标准文本模式的 RsID，以及 Codec2 700C 单载波语音**。
现有 2x8PSK1200F 的枚举、模式表、解调分支、发射入口及当前测试入口已移除；旧 AP580 协议、UART 驱动任务、配置项、播放代码和专用测试也已移除。
当前模式表共有 29 项：原来 28 项减去双载波 1200F，再加两个 PSKR 模式。

MFSK16 双向产品流程、MFSKpic、Olivia、RTTY 发射和完整 FreeDV 700D/700E 留到后续阶段。已有 `rx_decoder.c` 中的实验接收代码不作为本阶段已交付模式。历史文档和录音保留用于追溯，其旧模式验收结论不适用于此版本。

## 新增文本能力

PSK125R：125 Baud、K=7、1/2 FEC、多项式 `0x6d/0x4f`、2×2×40 交织、MFSK Varicode。
PSK250R：250 Baud、相同 FEC、2×2×80 交织。信息位速率分别为 62.5、125 bit/s，字符吞吐还要扣除 Varicode 开销。
它们复用已有 BPSK/FEC 路径，保留双 Viterbi 分支获取和连续流状态。
参数依据 [fldigi PSK 说明](https://www.w1hkj.org/FldigiHelp/psk_page.html) 以及本机 fldigi 的 `src/psk/psk.cxx`。

RsID 在 Text 中默认开启：一次发射开始前发送一次标识；接收端发现受支持的标识后切换接收模式和音频频率。
`RSID OFF` 关闭收发标识；`RSID ON` 重新开启。这不会在正在发送的内容中途插入标识。
实现普通 USB 方向的 primary RsID：GF(16) 的 RS(15,3)，15 个音调，音调间隔 11025/1024 Hz，符号周期 1024/11025 秒，前后各 5 个符号周期静音。

当前识别 BPSK31/63/125/250/500、QPSK31/63/125/250、PSK125R/250R。
接收搜索范围约为当前选定中心频率 ±32 Hz，频率网格约 2.69 Hz；允许最多一个符号错误并抑制重复通知。
这不是全频带搜索，不支持 extended RsID，也不识别未发送标识的未知信号。
Audio 和自定义图片流不发送标准文本标识；语音也不冒用 FreeDV 的 ID。
参照 [fldigi RsID 说明](https://www.w1hkj.org/FldigiHelp/id_configuration_page.html) 和 `src/rsid/rsid.cxx`、`rsid_defs.cxx`。

## 语音链路与使用

发送：板载音频输入 48 kHz → 144 tap FIR/6 → 8 kHz PCM → Codec2 700C → 二进制语音包 → 8PSK1200F（可选 8PSK1000F）→ 板载音频输出。

接收：板载音频输入 → PSK/FEC 解码 → 同步、描述字及 CRC 检查 → 有界播放缓冲 → Codec2 解码 → FIR×6 → 板载音频输出。
编码、解码都在 P4 上运行，不再需要 AP580；GPIO32/33 不再被语音服务占用。

上电仍是 Text/RX。LCD 选择 Audio，按住 **Hold to talk (PTT)** 发送，松开后排空并回到 RX。
也可以选择 Tx 持续讲话，用 Rx 或 `STOP` 结束。两个语音调制方式可在 LCD 或串口选择；网页目前配置接收端。
两端 Audio 的音频中心频率固定为 2000 Hz。

```text
CONTENT AUDIO
TXMODE 8PSK1200F
ROLE TX
ROLE RX
RXDIAG
```

`ROLE RX` 和 `STOP` 会停止采集，补齐不足 40 ms 的最后一帧，发送末尾语音包和 END，然后继续运行当前 FEC/交织流直至排空。
小包之间保持连续调制，队列暂空时只发送二进制零；每个 PTT 会话只训练一次。
控制层最多等待 5 秒，超时有日志并停止发送。该上限不是正常播放延迟或短波性能承诺。

板载 ES8311 的输入输出接口随半双工方向复用。发射时输入应接麦克风、输出送电台；接收时输入来自电台、输出送扬声器。
如果现有接线把它固定用作电台音频接口，需要按实际硬件配置音频切换或独立麦克风/扬声器接口，不能把解码语音回送电台。
屏幕 PTT 控制软件音频流，未配置未知 GPIO 去驱动电台的物理 PTT；实际电台需已有的 PTT/VOX 或外部控制路径。

## 二进制语音包 v1

每包固定 15 字节，不经过文本 Varicode，与旧 AP580 同步字和帧格式无关。

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 4 | 同步字 `C2 70 C2 A5` |
| 4 | 1 | 版本 bits7:6=`01`；编码 bits5:4=`01`（700C）；bit3 START；bit2 END；bits1:0 帧数 0～2 |
| 5 | 1 | 包序号，模 256 递增；新 PTT 从 0 开始 |
| 6 | 7 | 两帧各 28 bit，MSB first 紧密打包；单帧或空 END 的剩余位填 0 |
| 13 | 2 | CRC-16/CCITT-FALSE：poly `0x1021`、init `0xffff`、无反射、xorout=0，对偏移 4～12 计算，高字节在前 |

正常包包含两帧，每 80 ms 产生一包：15×8/0.08 = 1500 bit/s，尚未经过调制 FEC。
END 可带 0、1 或 2 帧；没有 END 的包必须带 2 帧。CRC 或描述字错误的包不交给 Codec2，解析器通过滑动窗口恢复字节同步。
底层二进制接收器仍负责比特级同步重新获取。

700C 每帧 320 点、40 ms、28 bit。选定模式扣除调制器 FEC 后的信息位速率：8PSK1200F 为 2461.54 bit/s（本项目 16000/13 Baud），8PSK1000F 为 2000 bit/s。
FEC 已在这个比较中扣过一次。该语音流不能通过 fldigi 文本解码直接播放。

接收端预缓冲 80 ms，再按 40 ms 节拍解码；小序号缺口填静音，迟到帧不重复占用已补偿的时间，重复/过旧包丢弃，较大缺口重新获取播放时基。
无 END 且断流时最多补三个静音帧；收到 END 后有界排空。播放缓冲最多 16 帧，其他队列也有限额并统计丢弃。
协议错误、编码/解码计数、补静音、队列丢失、Codec2 最大运行时间和音频写错误可通过网页状态、LCD 或 `RXDIAG` 检查。
启动训练、交织、排队和硬件音频缓冲都会增加总延迟；80 ms 仅是组包周期，不能视作端到端延迟。

网页保存 `.c2v` 文件，内容为 CRC 验证通过的上述 15 字节包流，包含 START/END 与序号。
它既不是裸 `.c2` 帧，也不是 WAV；最多保留网页内最近 65536 字节，文件可能从包中间开始，解析器需要先找同步字。

## 构建与验证

依赖版本和生成码本说明见 `components/codec2/README.md`。Codec2 源码和生成码本均已放入工程，固件构建不需要联网下载它。
本目录是复制工程，旧 `build` 缓存指向原项目；新的固件构建固定使用 `build-codec2`。

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_idf_build.ps1
powershell -ExecutionPolicy Bypass -File tools/run_rate_checks.ps1
powershell -ExecutionPolicy Bypass -File tools/run_content_checks.ps1
powershell -ExecutionPolicy Bypass -File tools/run_rsid_checks.ps1
powershell -ExecutionPolicy Bypass -File tools/preview_lcd.ps1
```

本阶段主机测试结果：

- PSK125R/250R 各 4096 字节完整载荷无差异，包含首尾标记；单载波 8PSK1200F 的 0/±20 Hz 完整载荷回归通过。
- 8PSK1200F、8PSK1000F 各 90 个连续语音包完整恢复，CRC 错包为 0，单帧 END 完整恢复并结束调制。
- 实际上游 Codec2 700C 编码/解码运行通过；不是空实现。组包/解包、所有单 bit 损坏、插字节、丢字节、分片及序号/计时回绕通过。
- 播放节拍、重复包、丢包补偿、迟到帧、END 丢失和新会话重启通过。
- 原有图片协议 96 行 CRC、9216 像素以及单载波音频闭环通过，图片不依赖已删除的双载波模式。
- RsID 固定向量、0/±20 Hz 音频识别、一个符号缺失及有限随机噪声拒绝通过。
- 实际 LVGL 页面主机渲染、29 个模式选择、两个语音模式筛选和 PTT 按下/释放事件通过；网页 JavaScript 语法检查通过。
- ESP-IDF 5.5.5 / ESP32-P4 完整编译通过。构建日志与测试结果保存在 `build/*-checks*.log`、`build/codec2-build-final.log` 等文件中。

尚未进行新固件烧录、麦克风到扬声器的双板实测、fldigi 双向真实音频互通、短波衰落/多径验证，以及 LCD/网络/存储同时运行的长时间测试。因此这里是已实现并通过主机回归的第一阶段，不是已完成商业验收的 FreeDV 产品。

## 下一阶段

先测量 P4 编解码时间、内部 RAM/PSRAM、丢队列、首音节及持续语音延迟，完成双板和 fldigi 的实际音频互通。
再单独接入 MFSK16 文本收发，按需求增加 Olivia 8/500。
短波语音后续评估完整 FreeDV 700D/700E 的同步、OFDM、LDPC 与帧结构，建立与官方 FreeDV 的双向测试。
参考 [FreeDV 技术说明](https://github.com/drowe67/codec2/blob/main/README_freedv.md)。
