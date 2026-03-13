# ESP32-S3 手机热点 + U盘文件管理（PlatformIO 版）

这个工程面向 ESP32-S3 开发板（USB-OTG Host），实现：

1. 开机后创建 Wi-Fi 热点；
2. 手机连接热点后访问板子网页；
3. 对插在板子上的 U 盘进行文件管理（列表 / 上传 / 下载 / 删除）。

---

## 功能概览

- 默认热点：
  - SSID: `ESP32S3-USB-DISK`
  - 密码: `12345678`
- 默认访问地址：`http://192.168.4.1`
- API：
  - `GET /api/list?path=/`：列目录
  - `POST /api/upload?path=/xxx.bin`：上传文件（二进制 body）
  - `GET /api/download?path=/xxx.bin`：下载文件
  - `DELETE /api/file?path=/xxx.bin`：删除文件

---

## 硬件要求

- ESP32-S3（支持 USB-OTG）；
- OTG Host 连接方案（如 OTG 转接线 / 支持 Host 的 Type-C 电路）；
- FAT/FAT32 格式 U 盘。

> 如果 U 盘功耗较大，请使用外部供电，避免供电不足导致识别失败。

---

## PlatformIO 使用方法

### 1) 安装 PlatformIO

- VSCode 用户：安装 **PlatformIO IDE** 插件；
- CLI 用户：安装 `platformio` 命令行工具。

### 2) 编译

```bash
pio run
```

### 3) 烧录

```bash
pio run -t upload
```

### 4) 查看串口日志

```bash
pio device monitor
```

---

## 使用步骤

1. 插入 U 盘；
2. 给板子上电；
3. 手机连接热点 `ESP32S3-USB-DISK`；
4. 手机浏览器打开 `http://192.168.4.1`；
5. 在网页中上传、下载、删除文件。

---

## 工程结构

- `platformio.ini`：PlatformIO 构建配置（ESP-IDF framework）
- `main/main.c`：Wi-Fi AP + USB MSC 挂载 + HTTP 文件管理 API + 简易网页
- `main/idf_component.yml`：`usb_host_msc` 组件依赖
- `sdkconfig.defaults`：基础默认配置

---

## 可选自定义

你可以在 `platformio.ini` 的 `build_flags` 中覆盖默认热点名和密码：

```ini
build_flags =
  -D WIFI_SSID=\"你的热点名\"
  -D WIFI_PASS=\"你的热点密码\"
```

