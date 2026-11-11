# AutoLink 集成说明

GrainSilo 随固件源码提供 `firmware/libraries/AutoLinkCore` 和 `firmware/libraries/GSProto`，两库基于 [AutoLink v2.2.0](https://github.com/sun2006-xin/autolink/tree/v2.2.0)。GrainSilo 的 Master 叠加采样接口、READ_INFO 调度、离线回访和传感器错误应答四项集成改动。随附库已经包含这些改动。

使用 Espressif Arduino ESP32 core 3.3.10。S3 OLED 需要 Adafruit SSD1306、Adafruit GFX Library 和 Adafruit BusIO。使用 Arduino IDE 时，把两个随附库目录安装到 Arduino 用户库目录，并确认 IDE 选择的是本仓库提供的版本。

从仓库根目录运行以下 Arduino CLI 构建命令。S3 的 Flash 和 PSRAM 选项按实际模组选取。

```text
arduino-cli compile --fqbn esp32:esp32:esp32s3 --library firmware/libraries/AutoLinkCore --library firmware/libraries/GSProto firmware/master
arduino-cli compile --fqbn esp32:esp32:esp32c3 --library firmware/libraries/AutoLinkCore --library firmware/libraries/GSProto firmware/node
```

AutoLink 管理 RS-485 通信。ESP32 适配层连接 UART、计时和持久化。Windows 中心站通过局域网接收 S3 上报，可以独立运行。
