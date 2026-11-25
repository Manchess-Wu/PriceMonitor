# PriceMonitor

运行在 Linux（树莓派 4B / x86 服务器）上的个人商品价格监控系统。

- 后端：C++17（Drogon + libcurl + Lexbor + SQLite3 + nlohmann/json）
- 前端：单页 HTML（内嵌 CSS/JS，ECharts 走势图）
- 平台：京东、淘宝、天猫
- 部署：CMake 构建，systemd 托管，24 小时常驻

---

## 一、功能

- 粘贴商品链接即可加入监控，自动识别平台与商品 ID，自动跟随短链（u.jd.com、3.cn、m.tb.cn 等）
- 默认每 60 分钟采集一次，多商品并发、互不阻塞
- 完整保存历史价格（时间戳、现价、划线价、库存），支持任意时间范围查询
- Web 管理界面：增删改查、启停、手动采集、搜索筛选、价格曲线、目标价参考线
- 降价通知：命中目标价或跌幅超过阈值时推送（Server酱 / 企业微信机器人）
- 淘宝、京东通过本机 Chromium 采集，可绕过纯 HTTP 客户端无法通过的 JS 风控
- 单文件 SQLite 存储（WAL 模式），断电恢复友好

---

## 二、目录结构

```
PriceMonitor/
├── CMakeLists.txt
├── config/config.example.json      # 配置模板（首次启动也会自动生成）
├── deploy/
│   ├── install.sh                  # 依赖安装 / 构建 / 安装 / 注册 systemd
│   ├── login.sh                    # 登录助手：打开淘宝、京东登录窗口并保存登录态
│   └── pricemonitor.service        # systemd 单元
├── src/
│   ├── main.cc                     # 入口：加载配置、注册路由、启动调度器
│   ├── controller/ApiController.*  # REST API
│   ├── core/                       # Config / Database / HttpClient / HtmlParser / Scheduler / AppContext / Logger / Util
│   ├── model/Models.h              # Product / PriceRecord / JSON 序列化
│   ├── platform/
│   │   ├── BrowserClient.*         # 启动 Chromium + CDP 取回渲染后页面
│   │   ├── JdAdapter.*             # 京东（p.3.cn + 浏览器兜底）
│   │   ├── TaobaoAdapter.*         # 淘宝 / 天猫（浏览器解析）
│   │   ├── UrlDetector.*           # 链接识别与规范化
│   │   └── PlatformRegistry.cc     # 适配器工厂
│   └── service/                    # CrawlService / Notifier
├── web/index.html                  # 管理界面
└── data/                           # 运行时生成：数据库、浏览器登录态
```

---

## 三、环境要求

| 项目 | 要求 |
| --- | --- |
| 系统 | Debian 11+ / Ubuntu 20.04+ / Raspberry Pi OS（arm64） |
| 编译器 | GCC 9+（C++17） |
| 内存 | 采集时峰值约 400MB（浏览器），树莓派 4B 2GB 可用 |
| 磁盘 | 程序 + 依赖约 500MB，含 Chromium |
| 网络 | 可访问 apt 源、GitHub；采集需访问 `item.jd.com`、`item.taobao.com` |
| 图形环境 | 仅首次登录需要（桌面、VNC 或 `ssh -X`）；日常采集无需 |

程序运行依赖的库：Drogon、Lexbor、libcurl、SQLite3、nlohmann/json、OpenSSL、zlib、brotli、jsoncpp、uuid。

---

## 四、快速开始（本地开发）

```bash
# 1. 安装依赖（含 Chromium、Lexbor、Drogon 源码构建，需要 root）
bash deploy/install.sh --deps

# 2. 编译
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# 3. 跑起来（首次运行会自动生成 config/config.json 和 data/）
./build/PriceMonitor

# 4. 浏览器打开
#    http://127.0.0.1:8080
```

本地运行时，配置、数据库、浏览器登录态都放在项目目录下的 `config/`、`data/`。

---

## 五、部署到树莓派 / 服务器（正式）

### 5.1 传输源码

在开发机上打包（不要带 `build/`、`data/`、`config/config.json`）：

```bash
tar --exclude='build' --exclude='data' --exclude='.git' \
    --exclude='config/config.json' \
    -czf PriceMonitor.tar.gz PriceMonitor
```

传到目标机后解压：

```bash
tar -xzf PriceMonitor.tar.gz
cd PriceMonitor
```

> 二进制不能跨机器复用（架构、动态库不同），必须在目标机重新编译。

### 5.2 安装依赖

```bash
bash deploy/install.sh --deps
```

该脚本会：

1. `apt-get` 安装编译工具与全部开发库
2. 安装 Chromium（淘宝/京东采集必需）
3. 从源码构建并安装 Lexbor
4. 从源码构建并安装 Drogon（关闭 MySQL/PostgreSQL/Redis 等无用组件）

树莓派上 Drogon 编译较慢，建议先扩容 swap 再执行。

### 5.3 编译并安装

```bash
bash deploy/install.sh
```

该脚本会：

1. 编译项目（`build/PriceMonitor`）
2. 创建系统用户 `pricemonitor`
3. 安装到 `/opt/pricemonitor`（可执行文件、web、config、data）
4. 注册并启动 systemd 服务 `pricemonitor`

检查服务状态：

```bash
sudo systemctl status pricemonitor
sudo journalctl -u pricemonitor -f
```

### 5.4 修改配置

```bash
sudo -u pricemonitor nano /opt/pricemonitor/config/config.json
```

最少只需要确认以下几项（其余保持默认即可）：

```json
{
  "server":  { "listen": "0.0.0.0", "port": 8080 },
  "database": { "path": "./data/pricemonitor.db" },
  "crawler": {
    "interval_minutes": 60,
    "concurrency": 2,
    "browser": {
      "enabled": true,
      "chrome_path": "chromium",
      "profile_dir": "./data/chrome-profile",
      "timeout_seconds": 45
    }
  },
  "notifier": { "enabled": false }
}
```

- `chrome_path` 填 `chromium` 即可；程序也会自动探测 `chromium`、`chromium-browser`、`google-chrome`
- 修改后重启：`sudo systemctl restart pricemonitor`

### 5.5 登录淘宝 / 京东（每台机器一次）

淘宝和京东的商品页都会下发 JS 做风控校验，纯 HTTP 客户端会被拦到验证页。因此需要用真实浏览器登录一次，把登录态保存到 `data/chrome-profile`：

```bash
bash deploy/login.sh            # 依次登录淘宝、京东
bash deploy/login.sh taobao     # 只登录淘宝
bash deploy/login.sh jd         # 只登录京东
```

脚本会自动：停止服务 → 清理残留浏览器进程与锁文件 → 打开登录窗口 → 等待你关窗 → 校验登录态 → 重启服务。

登录方式：窗口里扫码即可，登录成功后**直接关闭浏览器窗口**（不要最小化）。

没有显示器的机器，三种办法：

| 环境 | 做法 |
| --- | --- |
| 有桌面 / 装了 VNC | 直接在设备上执行 `bash deploy/login.sh` |
| 只有 SSH | `ssh -X user@设备IP`，再执行 `bash deploy/login.sh`，窗口显示在你电脑上 |
| 纯无头 | 在另一台同系统机器登录后，把整个 `data/chrome-profile` 拷过来，保持属主 `pricemonitor` |

单独使用 `--login`（脚本内部就是这么调的）：

```bash
./build/PriceMonitor --login           # 淘宝
./build/PriceMonitor --login jd        # 京东
./build/PriceMonitor --login jd /path/to/config.json
```

### 5.6 验证部署

```bash
# 健康检查
curl http://127.0.0.1:8080/api/health

# 添加一个商品（会立即采集一次）
curl -X POST http://127.0.0.1:8080/api/products \
  -H 'Content-Type: application/json' \
  -d '{"url":"https://item.jd.com/100012043978.html","targetPrice":1999,"note":"等降价"}'

# 查看是否拿到价格
curl http://127.0.0.1:8080/api/products
```

浏览器打开 `http://<设备IP>:8080`，能看到商品卡片与走势图即部署成功。

---

## 六、日常运维

```bash
sudo systemctl status pricemonitor      # 状态
sudo systemctl restart pricemonitor     # 重启（改配置后）
sudo systemctl stop pricemonitor        # 停止
sudo journalctl -u pricemonitor -f      # 实时日志
sudo journalctl -u pricemonitor --since today
```

### 升级程序

```bash
# 在源码目录重新编译
cmake --build build -j"$(nproc)"

# 停止服务后替换二进制，再启动
sudo systemctl stop pricemonitor
sudo install -m 755 build/PriceMonitor /opt/pricemonitor/PriceMonitor
sudo systemctl start pricemonitor
```

### 备份

```bash
sudo systemctl stop pricemonitor
sudo tar -czf pricemonitor-backup-$(date +%F).tar.gz \
    -C /opt/pricemonitor data
sudo systemctl start pricemonitor
```

`data/` 里包含 `pricemonitor.db` 和 `chrome-profile`（登录态）。备份前必须停服务，避免 WAL 未落盘。

### 迁移到新机器

1. 按 5.1～5.4 在新机器部署
2. 拷贝数据库：`data/pricemonitor.db` → 新机器 `/opt/pricemonitor/data/`
3. 想省掉重新登录，可拷贝 `data/chrome-profile`，并修正属主：
   ```bash
   sudo chown -R pricemonitor:pricemonitor /opt/pricemonitor/data
   sudo chmod 700 /opt/pricemonitor/data/chrome-profile
   ```
4. 若采集提示登录失效，直接在新机器执行 `bash deploy/login.sh`

---

## 七、配置项说明

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `server.listen` / `port` | `0.0.0.0` / `8080` | 监听地址与端口 |
| `server.threads` | 4 | Drogon 工作线程数，树莓派建议 2～4 |
| `server.document_root` | `./web` | 前端目录，需包含 `index.html` |
| `database.path` | `./data/pricemonitor.db` | SQLite 文件，目录自动创建 |
| `crawler.interval_minutes` | 60 | 采集间隔（分钟） |
| `crawler.timeout_seconds` | 20 | 单次 HTTP 超时 |
| `crawler.concurrency` | 2 | 并发采集数，建议不超过 2 |
| `crawler.user_agent` | Chrome UA | 全局 User-Agent |
| `crawler.jd.cookie` | 空 | 可选，接口需要登录时使用 |
| `crawler.taobao.cookie` | 空 | 可选，关闭浏览器模式时的 HTTP 兜底 |
| `crawler.browser.enabled` | true | 启用浏览器采集（淘宝/京东必需） |
| `crawler.browser.headless` | true | 是否无界面运行；遇到京东频控页时可尝试设为 false（需图形环境） |
| `crawler.browser.chrome_path` | `chromium` | Chromium/Chrome 路径，找不到时自动探测 |
| `crawler.browser.profile_dir` | `./data/chrome-profile` | 浏览器配置目录，保存登录态 |
| `crawler.browser.user_agent` | Chrome UA | 浏览器 UA，建议与登录时一致 |
| `crawler.browser.timeout_seconds` | 45 | 页面加载超时 |
| `notifier.enabled` | false | 是否启用降价通知 |
| `notifier.type` | `serverchan` | `serverchan` 或 `wecom` |
| `notifier.send_key` | 空 | Server酱 SendKey（`SCT...`） |
| `notifier.wecom_webhook` | 空 | 企业微信群机器人 Webhook |
| `notifier.drop_percent` | 5.0 | 较上次跌幅达到该百分比时提醒，0 表示关闭 |

---

## 八、采集原理

### 淘宝 / 天猫

商品页会先返回一段 JS，由浏览器计算设备指纹并换取风控凭证；纯 HTTP 客户端无法执行 JS，因此永远拿到验证页。本项目的做法是：

```
启动 Chromium（--headless=new --user-data-dir=data/chrome-profile）
  → CDP 打开商品页
  → 等待 priceText / 商品价格节点出现
  → 取回渲染后的 HTML
  → 解析 priceText、shopName、images、库存
  → 关闭浏览器
```

浏览器只在采集时启动，几十秒后自动退出，不常驻内存。登录态过期时会返回「淘宝登录已失效，请执行 ./PriceMonitor --login 重新登录」。

### 京东

```
优先请求 p.3.cn/prices/mgets 价格接口（快，通常无需登录）
  → 失败时用同样的浏览器方式打开 item.jd.com
  → 解析 class="product-price--value" / class="price J-p-xxx" / 内嵌 JSON
```

商品标题、主图、店铺来自详情页解析。若 `p.3.cn` 被网络环境劫持（例如校园网把它解析到内网 IP），会自动走浏览器兜底，只是速度慢一些。

### 采集策略

- 默认 1 小时/次，单商品串行，多商品并发上限 2
- 不做验证码破解、不模拟人工操作、不高频轮询
- 页面结构与接口可能调整，适配逻辑集中在 `src/platform/*Adapter.cc`

---

## 九、REST API

所有接口返回 `{"ok": true, "data": ...}` 或 `{"ok": false, "error": "..."}`。

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/health` | 健康检查 |
| GET | `/api/stats` | 商品数、记录数、24h 采集量、间隔、Cookie 状态 |
| GET | `/api/config` | 只读配置摘要（不含密钥） |
| GET | `/api/products` | 商品列表（含最近价、上次价、涨跌幅） |
| POST | `/api/products` | 添加商品 `{url, targetPrice?, note?}`，同步完成首次采集 |
| GET | `/api/products/{id}` | 商品详情 |
| PUT | `/api/products/{id}` | 修改 `{targetPrice?, note?, enabled?}` |
| DELETE | `/api/products/{id}` | 删除商品及其价格记录 |
| POST | `/api/products/{id}/refresh` | 立即采集一次 |
| GET | `/api/products/{id}/prices` | 历史价格，参数 `limit`、`days`、`from`、`to`（epoch 秒） |

```bash
curl "http://127.0.0.1:8080/api/products/1/prices?days=30"
```

---

## 十、数据表

```sql
products(
  id, platform, product_id, url, title, shop_name, image_url,
  target_price, note, enabled, created_at, updated_at,
  last_checked_at, last_error, last_notified_price,
  UNIQUE(platform, product_id)
)
price_records(
  id, product_id REFERENCES products(id) ON DELETE CASCADE,
  price, original_price, in_stock, captured_at, raw
)
crawl_logs(id, product_id, ok, message, created_at)
```

时间字段均为 epoch 秒（展示时按本地时区格式化），价格用 REAL 存储、展示保留两位小数。

---

## 十一、常见问题

| 现象 | 处理 |
| --- | --- |
| `No CMAKE_CXX_COMPILER could be found` | 未装编译器：`sudo apt-get install -y build-essential`，然后 `rm -rf build` 重新配置 |
| 链接报 `undefined reference to sqlite3_*` | 旧版 `/usr/local/lib/cmake/Drogon` 里带了一个过时的 `FindSQLite3.cmake`；本项目已改为直接探测库，重新 `cmake` 即可 |
| 页面 500「未找到 web/index.html」 | `server.document_root` 不对，或安装时漏拷 `web/` |
| 淘宝提示「未配置 Cookie」 | 浏览器模式没启用；检查 `crawler.browser.enabled` |
| 淘宝/京东提示「登录已失效」 | 执行 `bash deploy/login.sh` 重新登录 |
| 提示「浏览器启动超时」 | 检查 `chrome_path`、配置目录是否被残留 Chrome 占用 |
| 提示「浏览器启动后立即退出」 | 没有图形界面（DISPLAY）；用 VNC/`ssh -X` 执行登录 |
| 京东提示「未返回有效价格」 | `p.3.cn` 不可达且浏览器兜底也没拿到价格，检查登录态与商品是否下架 |
| 时间显示不对 | systemd 单元里保留 `Environment=TZ=Asia/Shanghai` |
| 树莓派编译 Drogon 很慢/内存不足 | 扩容 swap，或用上面的最小化编译参数 |

---

## 十二、安全提示

- Web 界面**没有登录鉴权**，默认监听 `0.0.0.0:8080`。仅建议在局域网使用；需要公网访问时，用 Nginx 反向代理并加 HTTP 鉴权或 VPN。
- `data/chrome-profile` 等同于淘宝/京东账号登录凭证，权限必须保持 `700`，不要提交到 Git、不要放到共享目录。
- `config/config.json` 可能包含 Cookie 与通知密钥，同样不要提交到公开仓库。

---

## 十三、后续可扩展

- 更多平台适配器（拼多多、亚马逊等）：实现 `PlatformAdapter` 并注册
- 微信服务号 / 钉钉 / Telegram 通知渠道
- 价格任务暂停区间、折扣统计报表
- 多用户与访问鉴权、Prometheus 指标导出
