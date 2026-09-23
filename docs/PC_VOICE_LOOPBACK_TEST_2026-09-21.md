# PC 实时语音回环测试工具（host_voice_loopback）

日期：2026-09-21

## 用途

在 PC 上实时验证本工程 Codec2 语音 + PSK modem 全链路：从电脑**麦克风**采集语音，经
**内部真实的固件 DSP**（Codec2 700C 编码 → 8PSK 调制 → 解调 → Codec2 解码），从**扬声器**
输出还原后的语音。可加信道噪声/频偏做压力测试，也可无音频设备自测（`--selftest`）。

链路与 ESP32-P4 板载语音完全一致（逐块对应）：

```
麦克风 48 kHz
  → voice_downsample 降采样 8 kHz
  → Codec2 700C 编码（28 bit / 40 ms）
  → voice_protocol 双帧打包（同步字 0xC270C2A5 + 序号 + CRC，15 字节/80 ms）
  → psk_tx 8PSK1200F/8PSK1000F 调制（48 kHz，2000 Hz 载波，二进制流）
  → [可选 AWGN 信道 / RX 调谐偏移模拟频偏]
  → audio_decim（DC blocker）→ psk_rx 解调（二进制同步字）
  → voice_wire_feed 解包 → voice_playout 去抖/丢帧隐藏
  → Codec2 解码 → voice_upsample 升采样 48 kHz → 扬声器
```

复用源码：`components/codec2`、`components/psk_rtty`、`main/psk_tx.c`、`main/psk_rx.c`、
`main/psk_codec.c`、`main/psk_modes.c`、`main/voice_protocol.c`、`main/voice_playout.c`、
`main/voice_resample.c`、`main/audio_decim.c`。

## 构建

```powershell
# 需要 Cygwin gcc（codec2 vendor 源码使用 C99 VLA，MSVC 无法编译，与 run_voice_checks.py 一致）
& .\tools\build_host_voice_loopback.ps1
```

产物：`build/host-tools/host_voice_loopback.exe`（构建脚本会把 `cygwin1.dll` 复制到 exe
旁边，可独立双击运行，无需把 Cygwin 加入 PATH）。

## 运行

```powershell
# 实时回环（麦克风 → 扬声器），佩戴耳机避免回声；按 q 或 Ctrl+C 停止
.\build\host-tools\host_voice_loopback.exe

# 无音频设备自测（合成语音跑通全链路，打印 PASS/FAIL，退出码 0/1）
.\build\host-tools\host_voice_loopback.exe --selftest 10

# 列出音频设备索引
.\build\host-tools\host_voice_loopback.exe --list-devices

# 压力测试：AWGN 6 dB + 频偏 25 Hz + 输出调试 WAV
.\build\host-tools\host_voice_loopback.exe --noise-db 6 --rx-offset 25 --dump .\loop
```

## 选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `--mode NAME` | `8PSK1200F` | 语音链路仅支持 `8PSK1200F` / `8PSK1000F`（与固件一致） |
| `--tone HZ` | `2000` | TX 载波频率（300..3500） |
| `--rx-offset HZ` | `0` | RX 调谐偏移，模拟收发频偏（±100），由 AFC 纠正 |
| `--noise-db X` | 关 | 信道 AWGN，X 为相对音调 RMS 的 SNR（0..60） |
| `--mic N / --speaker N` | 系统默认 | 音频设备索引，`--list-devices` 查看 |
| `--mic-gain-db X` | `0` | 麦克风增益（-40..40） |
| `--output-gain X` | `1.0` | 扬声器输出增益（0..2） |
| `--duration SEC` | 持续 | 运行 SEC 秒后自动停止 |
| `--dump PREFIX` | 关 | 输出 `PREFIX_tx.wav`（已调制）、`PREFIX_rx_in.wav`（接收信号）、`PREFIX_rx_out.wav`（还原语音） |
| `--selftest SEC` | — | 无音频设备自测（合成语音，虚拟时钟，满速运行） |

## 状态行 / 汇总字段

- `TXpkt / RXpkt / RX帧`：发送/接收语音包数及帧数（CRC 通过才计数）
- `crc / bad / sync`：CRC 错误、坏描述符、同步丢失字节
- `dec / conc / drop / lost / dup`：解码帧、隐藏帧、播放缓冲丢弃帧、丢包、重复
- `lock`：解调器锁定状态（3 = 已锁定）
- `wirelat / e2e~`：编码→解包延迟（含调制解调固有交织延迟）与估计端到端延迟

## 验证结果（2026-09-21）

| 场景 | 结果 |
|---|---|
| `--selftest 10` 干净信道 | PASS：126/126 包，CRC 0，解码 223 帧，隐藏 0 |
| `--selftest 10 --noise-db 6` | PASS：126/126 包，CRC 0（同步重扫 99 字节，无丢包） |
| `--selftest 10 --rx-offset 25` | 如实报告 FAIL：123/126 包，CRC 错 3（AFC 限值 ±50 Hz，25 Hz 属严重压力） |
| 调试 WAV 频谱 | tx 为恒定包络 PSK 载波；rx_out 为基频 ~215 Hz 的谐波语音（典型 700C 声码器输出） |

已知现象：
- **启动突发丢弃帧**：训练段（前 ~1.7 s）TX 积压的语音包在锁定后以高于实时的速率上链，
  接收端播放缓冲（容量 16 记录）溢出丢弃最旧帧（`drop` 约 27 帧）。这是调制解调器的固有
  行为，板载固件同样存在，属正常现象。
- **端到端延迟**：8PSK1200F 交织深度 512 对带来 ~0.6 s 固有解码延迟；总延迟约 0.8~1 s。
- 实时模式请佩戴耳机，避免扬声器语音回灌麦克风形成回声环。
