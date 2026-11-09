# 客厅 RF 中枢

> 用一块 ESP32-S3 + ESPHome，把三套互不兼容的老式射频设备接进 Home Assistant：昇辉自发电开关、433 MHz 风扇灯、nRF24 电动沙发。核心设计是**完全无状态**：RF 单向发射、灯具只认「切换」帧，因此不推算、不维护任何开 / 关状态——任何来源的命令都只发射一次对应 RF 码。

---

## 它解决了什么问题

| 痛点 | 处理方式 |
| --- | --- |
| 原厂 RF 开关只发射、不回传，HA 无从知道灯是开是关 | 不推算状态：`turn_on` / `turn_off` 不做分辨，都只发射一次切换码；界面也不显示开关状态 |
| 一盏灯由两个物理开关控制，两个开关发的码不一样，语音会反相 | 「一槽多码」：一个主槽位可绑多个副码，命中任一码都按同一个槽位 ID 上报 |
| 固定 8 个槽位不够，且改数组长度会丢 NVS 旧数据 | 变长 NVS blob，槽位数量无固定上限；旧 8 槽在首次启动时无损迁移 |
| 网页里录入的开关不会自动变成 HA 实体 | 手写 MQTT Discovery，录入 / 改名 / 删除即时同步成 HA 的 button 与 event |
| 物理开关连按两次，HA 只能看到一次 | 用 MQTT `event` 实体上报每次按压，而不是只看 on/off 状态 |
| ESPHome 自带首页没有开关管理界面 | 在官方 v3 首页注入脚本，新增「昇辉开关管理」区块与 `/sw/api` 接口 |

---

## 系统架构

```mermaid
flowchart TB
    subgraph RF["射频设备 · 单向，无回传"]
        A1["昇辉自发电开关<br/>433.3 MHz / 2-FSK"]
        A2["风扇灯遥控器<br/>433.92 MHz / OOK"]
        A3["电动沙发遥控器<br/>2.4 GHz / nRF24"]
    end

    subgraph ESP["ESP32-S3 · ESPHome 固件 · 无状态"]
        B1["CC1101 接收"]
        B2["T2L / nRF24 发射"]
        B3["slot_store<br/>变长 NVS blob + 副码表"]
        B4["Web 管理页<br/>/ 与 /sw/api"]
    end

    subgraph HA["Home Assistant · MQTT"]
        C0["mosquitto broker"]
        C1["Discovery 生成的<br/>button / event"]
        C2["template switch × 8<br/>optimistic，仅显示"]
        C3["槽位 9 联动米家原生灯"]
        C4["巴法云 → 小爱语音"]
    end

    A1 --> B1 --> B3
    B3 -->|"发射一次 RF 码"| B2
    A1 -.->|"按压脉冲 event"| C0
    B4 -->|"触发 / 管理"| B3
    C0 --> C1
    C1 -->|"N/command"| B3
    C0 --> C3
    C2 -->|"on 或 off 都按一次按钮"| C1
    C2 --> C4
    B2 -.->|"发射 RF"| A2
    B2 -.->|"发射 RF"| A3
```

---

## 功能特性

- **动态昇辉槽位**：网页填名称 → 30 秒内连按 2~3 下物理开关完成录入；可随时改名 / 删除 / 触发。槽位 ID 单调递增、删除后永不复用（HA 实体与自动化按 ID 绑定，复用会串到旧实体）。
- **双控副码**：每个槽位可再绑多个 RF 帧，命中副码仍按主槽位 ID 上报，HA 侧实体、topic、自动化完全不用改。
- **抗噪声学习**：CC1101 在空闲时每 0.5~2 秒会收到随机噪声包，原「连续两帧相同」会被噪声插队打断。改为 3 秒滑动窗口内找重复帧（允许 1 字节误码），并要求连按 2~3 下。
- **官方风扇卡片**：`template fan`，6 挡滑块 + 正反转 + 开关；HA 滑块松手才下发一次 `speed_level`，设备收到即刻发射，不再等待 250 ms。
- **彻底无状态**：设备不存状态镜像、HA 不用 `input_boolean`、没有 `state/all` 广播和对表自动化。HA 的 `template switch` 用内建 `optimistic` 只给界面 / 云端一个显示值，`turn_on` 与 `turn_off` 都无条件按一次按钮发射。首页与状态页都不显示开关状态。
- **中文管理界面**：中文分组标题、中文运行时长、调试日志与实体表等高、日志内部滚动且新消息自动到底（手动上翻时不打断）。
- **MQTT 掉线不重启设备**：`reboot_timeout: 0s`，射频中枢不会因为 broker 抖动而重启。

---

## 界面预览

### 设备自带管理页（`http://<设备 IP>/`）

![ESP32 首页：传感器与控制、调试日志、昇辉开关管理](docs/device-web-ui.png)

> 在 ESPHome 官方 v3 首页上注入的「昇辉开关管理」区块。上排左为静态实体（沙发 8 个动作、`template fan` 6 挡滑块），右为与之等高、内部滚动且自动跟随的调试日志；中间是动态槽位列表：副码徽标、触发 / 改名 / 添加副码 / 清除副码 / 删除；底部「最近收到的信号」显示现场抓到的帧与 RSSI，用于判断学习时收到的是开关信号还是噪声。标题右侧是中文运行时长。（截图摄于状态圆点与「状态对表」卡片移除之前，仅用于展示区块布局。）

### Home Assistant 侧边栏「昇辉灯状态」

![HA 侧边栏：已接入小爱的开关与客厅灯](docs/ha-shenghui-status.png)

> 截图摄于界面精简之前：当时左侧 8 个 `input_boolean` 用于对表、右侧 8 个 `template switch` 是接入小爱的开关（**点击会发射 RF**）。当前版本已移除对表卡片、全部假状态与设备首页的状态圆点；右侧开关仍在，只是不再分辨开 / 关，点一次就发射一次。

---

## 硬件与接线

主控为 ESP32-S3（`esp32-s3-devkitc-1`，Arduino 框架）。数字即 GPIO 编号：

| 模块 | 引脚 | 说明 |
| --- | --- | --- |
| CC1101（昇辉接收） | GDO0=4, CSN=5, SCK=14, MOSI=13, MISO=12 | 3V3 供电；硬件 SPI 由 `spi:` 配置 |
| T2L（风扇灯发射） | DAT=7 | 3V3 供电；不要用 19/20（USB 占用） |
| nRF24 / SI24R1（沙发） | CE=15, CSN=16, SCK=17, MOSI=18, MISO=21 | 软件 SPI，零外部库依赖；3V3 加 10 µF + 100 nF 退耦 |

**射频参数**

| 子系统 | 频率 / 调制 | 关键参数 |
| --- | --- | --- |
| 昇辉 | 433.3 MHz / 250 kbps 2-FSK | 频偏 90 kHz，同步字 `54 21`，负载 `A4 23` + 8 字节码，触发连发 4 次；接收带宽 650 kHz |
| 风扇灯 | 433.92 MHz / OOK | rc-switch protocol 1，pulse 250 µs，单帧 40 ms，重发帧数由 `substitutions.fanlight_repeat` 控制，11 组码 |
| 沙发 | 2480 MHz / 1 Mbps / 0 dBm | 地址 `05 DC 03 00 00`，DPL、免确认，8 组动作帧 |

> 风扇灯单帧精确 40 ms，帧间无额外静默（`wait_time: 0s`）。帧数由 `config/esp32.yaml` 顶部的 `substitutions.fanlight_repeat` 单点控制。实测接收器必须收到**一组连续帧**才会解码，帧数是决定性参数：
>
> | 帧数 / 时长 | 现象 |
> | --- | --- |
> | 40 帧 / 1600 ms | 被当成 3~4 次按键，「点一次灯反复切换」 |
> | **6 帧 / 240 ms（默认）** | 点一次触发一次，历史验证可用 |
> | 3 帧 / 120 ms | 连点 9 次灯只动 2 次，临界不可靠 |
> | 1 帧 / 40 ms | 连点 9 次灯 0 次响应，接收器完全不认 |
>
> 原装遥控器快速连按每次都响应，说明差异不在接收器冷却期，而在 **ESP32 拼出的帧流结构**（帧数 / 帧间隙 / 结束帧）。要继续优化必须先抓原装波形比对，不要再盲调帧数。

---

## 目录结构

```
.
├── docker-compose.yml          # ESPHome Dashboard（官方镜像 + 统一编译缓存卷）
├── .env.example                # Dashboard 凭据模板
├── .gitignore                  # 凭据 / 缓存 / 历史快照的忽略规则
├── README.md
├── config/
│   ├── .gitignore              # Device Builder 本地状态与密钥的忽略规则
│   ├── esp32.yaml              # 唯一的主配置：声明式实体 + includes + on_boot 装配
│   ├── secrets.yaml.example    # 密钥模板（真实 secrets.yaml 不入库）
│   ├── sofa_rf_s3.h            # nRF24 软件 SPI + 沙发 8 帧
│   ├── fanlight_tx.h           # 风扇灯 11 组 OOK 码表 + 挡位映射
│   ├── slot_store.h            # 变长 NVS blob：槽位 + 副码 + 旧 8 槽迁移
│   ├── shenghui_switch.h       # CC1101 收包 / 学习状态机 / 去重 / 触发
│   ├── rf_admin.h              # 槽位命名 / 删除 / 学习入口（薄封装）
│   ├── shenghui_mqtt.h         # MQTT Discovery + 按压脉冲事件（无状态主题）
│   ├── shenghui_tab.h          # 首页接管 + /sw/api 接口
│   └── custom_index.h          # 补丁后的官方首页（gzip 字节数组，构建产物）
├── docs/
│   ├── device-web-ui.png       # 设备首页截图（README「界面预览」引用）
│   └── ha-shenghui-status.png  # HA 侧边栏状态页截图
└── tools/
    └── gen_index.py            # 从容器内官方首页生成 custom_index.h
```

**代码组织约定**：所有 `.h` 由 `esphome.includes:` 注入到 `main.cpp` **顶部**，早于任何 `id(...)` 对象声明。因此头文件一律不直接引用 `id(...)`，对象通过 `on_boot` 的 lambda 以参数或回调传入。这是本项目最容易踩的坑。

---

## 快速开始

### 1. 启动 ESPHome Dashboard

```bash
cp .env.example .env                          # 改成自己的账号密码
cp config/secrets.yaml.example config/secrets.yaml   # 填入 WiFi / MQTT / 后台密码
docker compose up -d
# Dashboard: http://<Docker 主机>:6052
```

`docker-compose.yml` 显式声明了 PlatformIO / IDF 的缓存变量。原因：镜像 `entrypoint.sh` 用 `export` 设置这些变量，只对 Dashboard 主进程生效；`docker compose exec` 起的进程读的是容器 `Config.Env`，会回落到另一份缓存目录，两条路径交替写同一个 `CMakeCache.txt` 会报 `source does not match the source used to generate cache`。

### 2. 生成首页头文件

`custom_index.h` 派生自容器内的官方 `server_index_v3.h`。仓库里附带已生成的版本，clone 后可直接编译；**升级 ESPHome 镜像后必须重新生成**：

```bash
docker cp tools/gen_index.py esphome:/tmp/gen_index.py
docker exec esphome python3 /tmp/gen_index.py
```

脚本对官方压缩 JS 做三处精确替换（中文分组标题、紧凑风扇控件、中文运行时长），**每处要求且只要求一次匹配**；官方页面一改动就会明确报错，而不是悄悄产出坏页面。

### 3. 首次刷写与后续 OTA

首次必须 USB 刷入一次（让设备拿到 WiFi 与固件），之后走 OTA：

```bash
docker exec esphome esphome run /config/esp32.yaml --device <设备 IP> --no-logs
```

> Windows 下注意：Git Bash 会把 `/config/...` 当路径转换，OTA 命令请用 PowerShell 执行。mDNS 不可达时显式给 `--device`。

### 4. 打开管理页

浏览器访问 `http://<设备 IP>/`，digest 认证用 `secrets.yaml` 里的 `web_username` / `web_password`。在「昇辉开关管理」里填名称、点「添加昇辉控股开关」、连按 2~3 下物理开关完成录入。

---

## 无状态发射链路（本项目核心）

RF 灯具只认「切换」帧，且没有状态回传。既然「开」和「关」都只能靠发射同一个码来切换，就没有任何需要分辨或保存的状态。所有入口收敛成一条规则：**一个命令 = 一次发射**。

| 入口 | 路径 | 动作 |
| --- | --- | --- |
| 物理昇辉开关 | CC1101 收包 → 按槽位查码 | 本机直接发射一次；另发一条非 retain `event` 脉冲供 HA 联动 |
| 网页「触发」 | `GET /sw/api?action=trigger&slot=N` | 设备发射一次，前端只发一个 HTTP 请求 |
| HA MQTT 按钮 | `shenghui/N/command` | 重放一次该槽位 RF 帧 |
| 米家 / 小爱（巴法） | 云端 `{"on": true}` 或 `{"on": false}` | 都映射到一次 `button.press`，不看当前状态 |

```mermaid
sequenceDiagram
    participant P as 物理开关 / 网页 / HA / 米家
    participant E as ESP32
    participant M as MQTT
    participant H as HA

    P->>E: 一次命令（不区分 on / off）
    E->>E: 查槽位码，发射一次 RF
    E-->>M: 可选：一条 event 脉冲（按压事件）
    M-->>H: 仅用于联动（如槽位 9 切换米家原生灯）
    Note over E,H: 无状态镜像、无状态广播、无对表、无回环判据
```

**为什么可以删掉状态**：早期用 `input_boolean` 推算状态，是为了给 HA 的 `template switch` 加 `condition` 门控，避免云端回显重复发射。现在回显在巴法集成的云端入口按时间窗合并，门控与假状态都不再需要；发射链路本身完全不看状态。

**唯一保留的合并**：巴法 / 米家云端会把一次点按回显成一串命令（实测间隔 2~7 秒），`bemfa_cloud/tcp.py` 用固定 5 秒静默窗 + 2 秒快速连点放行把这串合并成一次。它不分辨 on / off 语义、不比较 HA 状态，也不在物理链路上；删掉它，米家点一次会触发多次。

---

## MQTT 主题

前缀 `living-room-rf-hub`，Discovery 前缀 `homeassistant`，节点 ID `living_room_rf_hub`。`slot` 为 1-based 槽位 ID。

| 方向 | 主题 | 载荷 | retain |
| --- | --- | --- | --- |
| 设备 → HA | `homeassistant/button/living_room_rf_hub_shenghui_<slot>/config` | Discovery 配置 JSON | 是 |
| 设备 → HA | `homeassistant/event/living_room_rf_hub_shenghui_<slot>_press/config` | event 实体配置 JSON | 是 |
| HA → 设备 | `living-room-rf-hub/shenghui/<slot>/command` | `PRESS`：发射一次该槽位 RF 帧 | 否 |
| 设备 → HA | `living-room-rf-hub/shenghui/<slot>/event` | `{"event_type":"press"}`：一次按压脉冲 | 否 |

**没有状态主题**：设备不订阅、不发布 `state/all` / `state/set`，也不缓存任何开 / 关值。旧版本留下的 retain `state/all` 已在切换时清空。

设备每次连上 broker 都会遍历 `1..next_id-1` 全量重发 Discovery：已录入的发 config，已删除 / 未录入的发空 payload，保证换过 broker 或重启后删掉的实体不会自己长回来。`mqtt.discovery: false` 关闭 ESPHome 自身的自动发现，避免风扇、沙发等静态实体在 HA 里出现重复。

---

## 网页管理接口

由 `shenghui_tab.h` 通过官方 `web_server_base::add_handler()` 挂载，自动继承 web_server 的 digest 认证。`GET /sw/api?action=...`：

| action | 参数 | 作用 |
| --- | --- | --- |
| `list` | — | 槽位 JSON：名称、副码数、学习状态、最近收到的 RF 帧 |
| `add` | `name` | 分配新 ID 并进入学习模式 |
| `rename` | `slot`, `name` | 改名（同步刷新 HA 实体名） |
| `remove` | `slot` | 删除槽位（ID 保留不复用） |
| `trigger` | `slot` | 发射一次该槽位 RF 帧，仅此而已 |
| `sync` | `slot` | 手动重发该槽位的 Discovery 配置 |
| `add_alias` | `slot` | 为该槽位学习副码（双控） |
| `remove_alias` | `slot` | 清除该槽位全部副码 |

前端注入脚本还做了：中文分组标题、紧凑风扇控件、标题右侧中文运行时长、日志列与实体列等高、日志内部滚动并自动跟随（手动上翻时暂停跟随）。**状态显示、状态轮询、状态接口、操作收敛等待已全部移除**：一次操作只发一个请求，录入 / 副码学习的确认轮询压到 150 ms。射频侧按槽位独立去抖 150 ms（只吃掉一次按压内部的重复帧，快速连点不再丢失），网页「触发」的自收帧只屏蔽该槽位 150 ms；这些是 RF 帧级防护，不是状态判断。

> `text_sensor.sw_status` 设为 `internal: true`：它只作为学习与收包提示的内部通道，既不在首页渲染，也不注册到 HA。C++ 里多个函数以 `StatusT*` 复用它，所以实体对象保留。

---

## Home Assistant 侧

HA 配置不在本仓库内（部署在另一台常开主机的 Home Assistant 配置目录），以下是当前使用的结构摘要，全部用占位名表示。

**1. 无状态开关**

不再有 `input_boolean`。`rf_template.yaml` 的 8 个 `template switch` 用 HA 内建 `optimistic` 只提供一个显示值，`turn_on` 与 `turn_off` 完全对称：

```yaml
- name: 书房灯
  unique_id: rf_shufang_deng
  optimistic: true
  turn_on:
    - action: button.press
      target:
        entity_id: button.<Discovery 生成的槽位 1 按钮>
  turn_off:
    - action: button.press
      target:
        entity_id: button.<Discovery 生成的槽位 1 按钮>
```

实体 ID 由 Discovery 按中文名生成，请在 HA「开发者工具 → 状态」里确认实际值。没有 `condition`、没有状态模板、没有回环判据：命令来了就按一次按钮。

**2. 自动化（当前 2 条）**

| 类别 | 条数 | 作用 |
| --- | --- | --- |
| 面板保活 | 1 | 定时点亮门口中控屏 |
| 原生灯联动 | 1 | `shenghui/9/event` → `homeassistant.toggle` 米家原生客厅灯 |

原先 27 条只为假状态存在的自动化（8 条物理按压状态同步、1 条 `state/all` 广播、16 条 `state/set` 对表、2 条槽位 9 对表）全部删除。物理昇辉开关与槽位 4 的风扇灯照明已在设备本地直通，不需要 HA 中转。

**3. 侧边栏状态页**

Lovelace 注册 `shenghui-status` /「昇辉灯状态」，指向 `shenghui_dashboard.yaml`。显示 8 个可发射 RF 的开关与客厅灯原生灯，不显示任何推算状态。

**4. 小爱语音**

通过 `bemfa_cloud` 集成把 8 个 `template switch` 同步为巴法云主题。云端会把一次点按回显成一串命令，集成内的 `tcp.py` 只按时间窗合并这串回显，不分辨 on / off、不比较 HA 状态。注意该集成的选项流「修改同步」**只会改 HA 本地配置**，云端改名要另调 `https://apis.bemfa.com/va/modifyName`；米家 / 小爱侧还有缓存，通常要解绑重加才会用新名字。另外米家「其他平台设备」不会在首页生成设备卡片，只能语音控制。

---

## 响应速度：物理开关本地直通

昇辉「餐厅灯」物理开关实际控制的是风扇灯照明（LIGHT 码）。早期链路是
`昇辉 RF → ESP32 → MQTT 4/event → HA 自动化 → ESPHome API button.press → 发射 LIGHT 码`，
一次往返实测约 1 秒，快速连点还会被 300 ms 去抖吃掉。

现在这段**完全下沉到设备**，`on_packet` 的 press 回调里直接按本机按钮：

```cpp
[](int slot) {
  if (slot == 4) id(fanlight_light_button).press();
  shenghui_mqtt::device().publish_press(slot);  // 一条脉冲，仅用于 HA 联动
}
```

- HA 侧那条「餐厅开关 → 风扇灯」自动化必须**删除**，否则会重复发射一次。
- 槽位 4 的三条入口（物理按压 / `.35` 网页「触发」/ HA MQTT 按钮）统一走 `fanlight_light_button`，都发 LIGHT 码，不再发灯不认的墙壁开关码。
- 配合短 burst（单帧 40 ms，帧数见 `substitutions.fanlight_repeat`）、按槽 150 ms 去抖、150 ms 自收屏蔽，单次按压到灯响应只剩射频发射时间。`fanlight_light_button` 的 `on_press` 会打一条 INFO 日志 `LIGHT 码发射一次`，可在 .35 调试日志里直接数设备实际发射了几次。
- 首页一次操作只发一个 HTTP 请求：设备发射完就结束，没有 `flip()`、没有 `state/set`、没有状态圆点、250 ms 轮询或 400 ms 收敛。`/sw/api?action=trigger` 为每次请求分配独立的 scheduler tag——ESPHome 的 `set_timeout` 会取消同 tag 的旧任务，固定 tag 会把发射阻塞主循环期间的快速连点合并成一次（实测点十几次只发一两次）。

---

## 已知限制

- **RF 单向无回传、切换型灯**：没有状态可维护，米家 / 小爱显示的开 / 关只是 HA 乐观值，可能与灯的实际状态不一致。说「打开」和「关闭」都会让灯切换一次，这是设计使然；需要绝对开 / 关语义就必须换成有回传的灯具。
- **巴法云端回显**：一次点按会被云端回显成一串命令，集成按固定 5 秒静默窗合并；间隔落在 2~5 秒的连续两次点按，第二次可能被合并掉。物理开关 / 网页 / HA 按钮链路完全不经过这一层。
- **风扇卡片是乐观状态**：`restore_mode: NO_RESTORE`，用原装遥控器改挡不会同步回来。不要用 `ALWAYS_OFF`——它只把 `state` 强制为 false，`speed` / `direction` 仍会从 Flash 恢复，`setup()` 里的 `publish_state()` 会误触发 `on_speed_set` / `on_direction_set`，导致每次上电都发一组 RF 把风扇点转。`on_direction_set` 另有 `state` 保护，风扇关着时不下发方向码。
- **槽位 ID 删除后留空洞**，这是为了让 HA 实体 / topic / 自动化绑定保持稳定，不是 bug。
- **`gen_index.py` 与 ESPHome 版本强绑定**：官方首页的压缩 JS 一变，精确替换就会失败并报错，需重新对照生成。
- **米家首页设备卡片**：这些实体是软件虚拟对象，没有在小米 IoT 平台注册的产品型号与设备证书，无法成为米家原生设备卡片；巴法云 / Matter 桥等云对云或第三方路径在当前国区米家也不生成首页卡片。
- **不要公网暴露** `6052`（Dashboard）、设备 `80`（Web）、`1883`（MQTT）；本设计假定全在局域网内。

---

## 安全说明

- 所有凭据集中在 `config/secrets.yaml` 与 `.env`，两者都已被 `.gitignore` 排除；仓库内只有 `*.example` 模板。
- `esp32.yaml` 的 WiFi、AP、Web 后台一律用 `!secret` 引用，不留明文。
- `config/.gitignore` 里 Device Builder 生成的 `.device-builder*` 等本地状态同样不入库。
- 迭代过程中留下的 `*.before-*` / `*.backup` 快照包含早期明文配置，已在根 `.gitignore` 里排除，提交前请确认没有误加。
- 如果曾经把明文密码提交进过 git 历史，单纯删文件不够，应更换对应密码。
- `docs/` 里的截图包含家庭房间命名与现场抓到的 RF 帧示例；把仓库改为公开前，请先确认这些信息是否愿意对外可见。

---

## 版权与许可

`config/custom_index.h` 由 `tools/gen_index.py` 从 ESPHome 官方 `web_server/server_index_v3.h` 派生，包含其首页资源的 gzip 字节，受 ESPHome 项目原有许可证约束；本项目其余代码为自建的射频与集成逻辑。若重新分发，请一并保留上游许可声明。
