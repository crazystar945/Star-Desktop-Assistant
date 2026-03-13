# ESP32-S3 手机热点 + U盘文件管理

这个示例工程用于 ESP32-S3 开发板，目标功能：

1. 板子启动后开启 Wi-Fi AP（热点）；
2. 手机连接该热点后访问 `http://192.168.4.1`；
3. 在网页里对接在板子 USB-OTG 口上的 U 盘进行文件管理（列表、上传、下载、删除）。

## 功能说明

- 热点参数（默认）：
  - SSID: `ESP32S3-USB-DISK`
  - 密码: `12345678`
- Web API：
  - `GET /api/list?path=/`：列目录
  - `POST /api/upload?path=/xxx.bin`：上传文件（请求体为二进制内容）
  - `GET /api/download?path=/xxx.bin`：下载文件
  - `DELETE /api/file?path=/xxx.bin`：删除文件

## 硬件要求

- ESP32-S3（支持 USB-OTG）开发板；
- 可作为 USB Host 的硬件连接方式（OTG 转接线/Type-C Host 方案）；
- FAT/FAT32 格式 U 盘。

> 注意：不同开发板的 USB 供电能力不同。若 U 盘功耗较高，建议外部供电。

## 编译与烧录（ESP-IDF）

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

## 使用步骤

1. 插入 U 盘；
2. 上电后，手机连接热点 `ESP32S3-USB-DISK`；
3. 浏览器打开 `http://192.168.4.1`；
4. 在页面中进行上传、下载、删除操作。

## 目录结构

- `main/main.c`：Wi-Fi AP、USB MSC 挂载、HTTP 服务与文件操作逻辑
- `main/idf_component.yml`：依赖 `espressif/usb_host_msc` 组件

## 后续可扩展

- 增加目录创建/重命名/递归删除；
- 增加上传分片与断点续传；
- 增加登录鉴权（例如简单 token）；
- 增加文件预览（图片/文本）。
