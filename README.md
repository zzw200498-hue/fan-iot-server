# fan-iot-server —— 温控风扇物联网服务端

STM32 设备端（`stm32-esp8266-iot-gateway` 仓库）的服务端，合并了原规划的"项目 1（epoll + SQLite）"与"项目 2（生产者-消费者线程管道）"。

```
ESP8266 ──TCP:9999──> epoll 网络线程 ──互斥锁+条件变量──> worker 线程 ──> SQLite
浏览器 ──HTTP:8080──┬───────────────^ 读库查询
                    └── 下行命令 ──> epoll 线程 ──> ESP8266 ──> STM32 执行
```

## 功能

- **上行落库**：接收 `{"temp":26.7,"humi":77.0,"fan":40}`，解析后写入 SQLite（WAL 模式）
- **历史查询**：`GET /api/history?n=20` 返回最近 n 条 JSON
- **远程控制**：`GET /api/fan?set=on|off|auto` 向 ESP8266 下发 `FAN=ON` 等命令
- **内置网页**：浏览器打开 8080 端口，3 秒自动刷新的数据表格 + 控制按钮
- **优雅退出**：Ctrl+C 后按层关停（HTTP → 队列 → worker → 数据库）

## 线程模型（对应多线程知识点）

| 线程 | 职责 | 涉及的知识点 |
|---|---|---|
| 主线程 | epoll 事件循环（accept / recv / 粘包切分） | 非阻塞 IO、epoll、TCP 粘包处理 |
| worker | 消费队列：解析 JSON → 写库 | 互斥锁、条件变量、虚假唤醒（while）、优雅退出 |
| http | HTTP 服务 + 下行命令 | poll 超时、SIGPIPE、多线程共享 fd 的加锁 |

线程间只通过两种机制交互：`msg_queue`（互斥锁+条件变量的有界队列，满则丢包保护网络线程）和 SQLite（每线程独立连接 + WAL + busy_timeout）。

## 编译与运行（在 Ubuntu 虚拟机里）

```bash
# 依赖（首次）
sudo apt install build-essential libsqlite3-dev

# 编译
cd /mnt/hgfs/stm32+Linux/fan-iot-server
make

# 运行（参数：TCP端口 HTTP端口 数据库文件）
./fan-iot-server              # 默认 9999 / 8080 / fan.db
./fan-iot-server 9999 8080 /var/lib/fan/fan.db
```

## 测试（不接硬件也能跑）

```bash
# 1. 模拟设备上报
echo '{"temp":26.7,"humi":77.0,"fan":40}' | nc localhost 9999

# 2. 查历史
curl "http://localhost:8080/api/history?n=5"

# 3. 远程开风扇（ESP 已连接时会收到 FAN=ON）
curl "http://localhost:8080/api/fan?set=on"

# 4. 浏览器打开监控页
firefox http://localhost:8080
```

## 配置

WiFi / IP 等设备侧配置在固件仓库的 `User/config.h`；`SERVER_IP` 填本机（虚拟机）IP，端口 9999 与本服务对应。

## 目录结构

```
fan-iot-server/
├── Makefile
├── include/          # 各模块头文件（接口先行，先读头文件再看实现）
│   ├── msg_queue.h   # 有界队列：互斥锁+条件变量
│   ├── tcp_server.h  # epoll 网络层
│   ├── worker.h      # 消费者线程
│   ├── db.h          # SQLite 封装
│   ├── http.h        # HTTP 查询/控制
│   └── json.h        # 极简 JSON 字段提取
├── src/              # 对应实现
└── systemd/          # 开机自启配置（可选）
```

## systemd 开机自启（可选）

```bash
sudo cp systemd/fan-iot-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now fan-iot-server
journalctl -u fan-iot-server -f      # 看日志
```
