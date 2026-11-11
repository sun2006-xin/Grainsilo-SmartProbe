<p align="center">
  <img src="docs/images/github-banner.png" alt="GrainSilo Smart Probe — 粮仓探杆综合检测系统" width="100%">
</p>

<h1 align="center">粮仓探杆综合检测系统</h1>
<p align="center"><strong>GrainSilo Smart Probe</strong></p>
<p align="center">模块化温湿度采集 · RS-485 汇总 · 局域网主动上报 · Windows 本地管理</p>
<p align="center">
  <img alt="MCU: ESP32-S3 and ESP32-C3" src="https://img.shields.io/badge/MCU-ESP32--S3%20%2B%20ESP32--C3-00796B?style=flat-square">
  <img alt="Current prototype sensors: three SHT31 nodes" src="https://img.shields.io/badge/Sensors-3%C3%97%20SHT31%20(current%20prototype)-B88935?style=flat-square">
  <img alt="RS-485 with MAX3485" src="https://img.shields.io/badge/Bus-RS--485%20%2B%20MAX3485-0B5563?style=flat-square">
  <img alt="AutoLink v2.2.0" src="https://img.shields.io/badge/Protocol-AutoLink%20v2.2.0-496A81?style=flat-square">
  <img alt="Apache-2.0" src="https://img.shields.io/badge/License-Apache--2.0-377A68?style=flat-square">
  <img alt="Local station: Python and SQLite" src="https://img.shields.io/badge/Station-Python%20%2B%20SQLite-377A68?style=flat-square">
  <img alt="Web: HTML, CSS, JavaScript" src="https://img.shields.io/badge/Web-HTML%20%2F%20CSS%20%2F%20JavaScript-2D7188?style=flat-square">
  <img alt="Visualization: Three.js and ECharts" src="https://img.shields.io/badge/Visualization-Three.js%20%2B%20ECharts-385C78?style=flat-square">
  <img alt="S3 actively reports over Wi-Fi on the local network" src="https://img.shields.io/badge/Network-Wi--Fi%20LAN%20(active%20upload)-68804D?style=flat-square">
</p>

> GrainSilo 软件和固件采用 Apache-2.0。仓库包含 Windows 本地中心站、S3/C3 固件和随固件构建的 AutoLink 库。[AutoLink 独立仓库](https://github.com/sun2006-xin/autolink)提供协议规范与冻结基线。指定硬件接线资料采用 CC BY 4.0。

## 项目简介

GrainSilo 是一套粮仓温湿度监测原型。设计方案为 **1 根探杆、3 个 ESP32-C3 / MAX3485 / SHT31 测量节点和 1 个 ESP32-S3 主机**。测量节点采用集成小电路板方案，当前实验使用桌面原型。

节点测得的是探杆周围空气的温度和相对湿度（%RH），不是粮食含水率。S3 汇总节点数据后，通过 Wi-Fi 主动上报到同一可信局域网中的 Windows 本地中心站；电脑端使用本地 SQLite 保存历史记录。

## 项目结构与数据流

测量节点（ESP32-C3、MAX3485、SHT31）→ RS-485 总线 → ESP32-S3 主机 → Wi-Fi 可信局域网 → Windows 本地中心站与 SQLite。节点本地 I²C/UART 连接不属于 RS-485 四芯主干；四芯主干为 A、B、VCC、GND。

Windows 中心站提供温湿度预测和历史评估，S3 固件提供设备端趋势估计。

## 硬件连接

<p align="center">
  <a href="docs/images/hardware-connection.png"><img src="docs/images/hardware-connection.png" alt="GrainSilo 硬件连接与数据流" width="960"></a>
</p>

当前连接关系：两芯供电进入 S3 侧；S3 侧将供电与 RS-485 A/B 组成四芯主干，四芯为 **A、B、VCC、GND**，三个节点分别从主干分支。I²C 传感器线、节点 MCU 与本地收发器之间的 UART / 方向控制线都属于模块内部连接，不在四芯主干中。

完整接线原则、固件 GPIO 映射、供电核验和网络安全边界见[系统架构与硬件连接](docs/系统架构与硬件连接.md)。接线图适合快速理解结构，不能替代核对实物板卡丝印、电源能力及现场布线。

## 两个网页

<p align="center">
  <a href="docs/images/web-pages.png"><img src="docs/images/web-pages.png" alt="Windows 电脑中心站与 S3 设备网页实景截图" width="960"></a>
</p>

上图展示项目中的真实网页界面。图片中的状态和读数只作为页面展示内容，不是随仓库发布的数据库、论文记录或当前在线状态。

- **Windows 本地中心站网页**：由桌面程序启动，管理仓库档案、粮堆、探杆、环境、告警与历史记录；使用本地历史训练共享温湿度模型、显示候选预测与数据导出。数据保存在本机 SQLite。风机、灯光和通风目前只是接口预留，不会控制真实设备。
- **S3 探杆设备网页**：首次配置时可连接探杆自带的 GrainSilo 配网热点，设置家庭 Wi-Fi 和电脑中心站地址；加入家庭网络后，可通过探杆的局域网 IP 打开设备网页，查看节点数据并配置设备参数。设备网页通过 HTTP 提供，当前没有登录认证。

电脑端是接收方，探杆 S3 是主动上报方；电脑端不会扫描或寻找探杆。两者需位于可互通的同一可信局域网。

## 仓房与粮堆参数化展示

<p align="center">
  <a href="docs/images/warehouse-models.png"><img src="docs/images/warehouse-models.png" alt="平房仓与圆筒仓参数化展示" width="560"></a>
</p>

软件按仓型和用户输入的尺寸、吨位、容重等参数估算几何与体积，用于直观展示和配置核对。参数化模型不是现场测绘结果；估算粮堆体积不等于实测外形。探杆物理位置须现场校准，不能根据 AutoLink 通信地址推断节点的上、中、下位置。

## 温湿度预测与告警

预测对象是探杆周围空气的温度与相对湿度。Windows Station 可以用历史节点数据训练轻量 MLP，并在天气数据可用时使用室外网格天气作为辅助输入。S3 本地趋势估计使用设备端样本。

中心站用按时间留出的数据比较模型候选值与持续性基线。当前候选输出用于观察。模型版本和历史变更见 [更新记录](CHANGELOG.md)。

## 下载并启动电脑端

[下载 Windows x64 便携版（ZIP）](releases/GrainSilo-Desktop-Windows-x64.zip)

1. 下载并解压整个 ZIP 文件夹。
2. 双击“粮仓集成监控中心.exe”。
3. 桌面程序启动本机服务并打开浏览器；保持程序运行，退出程序会停止中心站服务。

这是便携程序，不是独立原生浏览器内核应用；网页由本机服务提供。用户数据库和日志保存在 Windows 用户本地数据目录，不包含在源码仓库或 ZIP 中。源码包也随 ZIP 一并提供。更详细说明见[电脑端运行与数据说明](docs/电脑端运行与数据说明.md)。

## 固件与 AutoLink 边界

`firmware/` 包含 S3/C3 应用源码、ESP32 适配和随项目分发的 AutoLinkCore、GSProto。固件编译方式和 GrainSilo 的四项集成补丁见 [AutoLink 集成说明](docs/AutoLink集成边界.md)。Windows 中心站可以独立运行。

## 安全与隐私

S3 配网热点目前没有密码；S3 设备网页与电脑中心站当前均没有登录认证，设备网页使用未加密 HTTP。仅用于可信个人/实验局域网，避免在共享网络配置，不要将 TCP 80 或 TCP 8000 映射到公网。电脑端不会自动修改 Windows 防火墙。

仓库不包含用户的 SQLite 数据库、论文记录、真实设备 UID、Wi-Fi 凭据或本机网络配置。天气与地点搜索会访问外部服务并发送用户选择的仓库坐标；不使用天气功能时可以不配置坐标。更多说明见[外部服务与资源来源](docs/资源来源与外部服务说明.md)。

## 许可与署名

- **软件与固件**按 [Apache-2.0](LICENSE) 发布，允许依照许可证条款商用。
- **硬件结构与接线资料**：仅 docs/系统架构与硬件连接.md 及 docs/images/hardware-connection.png 中的原创硬件资料采用 CC BY 4.0，使用时须署名“花鱼鱼”、附许可链接并说明修改。该许可不覆盖软件、AutoLink、品牌或未明确授予的专利权。
- **项目品牌与其余宣传 / 概念图片**：不因本仓库公开而获得额外商标、背书或商业使用许可；除硬件接线图明确授予的范围外，相关图片权利由权利人保留。
- 第三方代码、组件和数据服务继续适用各自的许可与条款，见[第三方组件与外部服务说明](docs/第三方组件许可证说明.md)。

## 目录

- firmware/：GrainSilo S3 主机与 C3 节点应用集成源码
- station/：Windows 本地中心站、SQLite 服务与网页
- releases/：Windows x64 便携用户包及对应电脑端源码
- docs/：架构接线、运行说明、AutoLink 边界、第三方许可和外部服务说明
- LICENSE、LICENSE-HARDWARE.md、LICENSES/：软件、硬件资料和第三方许可
