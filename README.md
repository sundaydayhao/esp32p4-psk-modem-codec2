# ESP32-P4 PSK / Codec2 modem

基于 ESP32-P4 Function EV Board、ESP-IDF 5.5.5 的半双工数字通信工程，包含 LCD、Wi-Fi AP 网页、文本和 96×96 图片收发，以及软件 Codec2 700C 语音链路。

2026-09-09 第一阶段更新：

- 新增 **PSK125R、PSK250R**，接通 LCD、串口、网页及文本收发。
- 新增部分标准文本模式的 **RsID 收发与自动切换接收模式/音频频率**，搜索当前中心 ±32 Hz；可用 `RSID OFF` 关闭。
- **Codec2 700C 替换 AP580**：8 kHz PCM、40 ms/28 bit；两帧紧密打包，经独立同步字、序号和 CRC 的二进制协议收发。
- 语音默认 **单载波 8PSK1200F**，可选 8PSK1000F；LCD 提供按住讲话的 PTT，松开后发送尾帧并有界排空。
- **已删除 2x8PSK1200F 支持**。图片也改用保留的单载波路径，现有模式表共 29 项。

这是向 FreeDV 方向推进的第一阶段。当前语音空口是本项目的 Codec2/PSK 协议，**尚不能与 FreeDV 700D/700E 互通**。MFSK16、Olivia 和完整 FreeDV 调制/同步/纠错留到后续。

## 使用

上电默认 Text/RX。LCD 选择 Audio 后，按住 **Hold to talk (PTT)** 发送，松开回到 RX；也可使用 Tx/Rx 切换或串口：

```text
CONTENT AUDIO
TXMODE 8PSK1200F
ROLE TX
ROLE RX
RXDIAG
```

输入输出使用板载音频接口；发送时需麦克风输入、电台音频输出，接收时需电台音频输入、扬声器输出。现有电台接线需要相应的半双工音频切换路径。屏幕 PTT 控制软件发射，没有配置未知硬件 GPIO 驱动电台物理 PTT。无需 AP580、无需语音 UART。

连接热点 `ESP32P4-PSK`（默认密码 `pskmodem32`），浏览器打开 [接收页面](http://192.168.4.1)。网页提供模式/频率/内容选择、诊断、图片保存和 Codec2 包保存；`.c2v` 是本项目的编码包文件，不是 WAV。

## 构建与验证

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_idf_build.ps1
powershell -ExecutionPolicy Bypass -File tools/run_rate_checks.ps1
powershell -ExecutionPolicy Bypass -File tools/run_content_checks.ps1
powershell -ExecutionPolicy Bypass -File tools/run_rsid_checks.ps1
powershell -ExecutionPolicy Bypass -File tools/preview_lcd.ps1
```

构建输出位于 `build-codec2/psk_modem_p4.bin`。构建脚本使用当前工程目录，不再跳转到原来的 `esp32p4-psk-modem`；旧 `build` 下的历史固件不代表本版本。

烧录脚本同样默认使用 `build-codec2`，如需其他构建目录可传入 `-BuildDir`：

```powershell
powershell -ExecutionPolicy Bypass -File tools/flash_board.ps1 -Port COM21
python tools/check_board_stability.py --port COM21 --seconds 300 --exercise-voice
```

`COM21` 仅是本次实测端口。稳定性检查会短暂发射线路音频，然后恢复 RX。

已通过 P4 固件编译、PSKR 完整载荷、两种单载波连续语音包、协议损坏/丢包/尾帧、图片及 RsID 的主机测试，并验证了 LVGL 模式选择和 PTT 事件。2026-09-09 上板排查修复了 Codec2 首次初始化的 main 栈溢出，以及片内 RAM 不足导致的任务创建失败；Codec2 初始化、工作缓冲及任务栈已按 P4 内存条件调整。新链路的双板、fldigi、真实电台及并发长时间验收仍未完成。有限理想信道样本无错不能代表实际短波表现。

完整格式、实现边界、接线要求、测试结果与下一阶段安排见 [Codec2 迁移说明](docs/CODEC2_MIGRATION_2026-09-09.md)。依赖源码固定版本及码本再生成方法见 [Codec2 组件](components/codec2/README.md)。较早日期的文档与录音仅保留为原工程历史记录。

重启修复已烧录至 ESP32-P4 rev3.2，完成 300 秒单板运行及内容/模式切换回归，未再次重启或触发看门狗；两种语音模式编码正常且队列丢弃为 0。完整测量及尚未验证的部分见迁移说明的“实机重启排查”。
