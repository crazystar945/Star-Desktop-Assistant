#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"

// USB MSC Host component (managed component: espressif/usb_host_msc)
#include "msc_host.h"
#include "msc_host_vfs.h"
#include "usb/usb_host.h"

#define WIFI_SSID "ESP32S3-USB-DISK"
#define WIFI_PASS "12345678"
#define WIFI_CHANNEL 6
#define MAX_STA_CONN 4

#define BASE_PATH "/usb"
#define SCRATCH_BUFSIZE 4096

static const char *TAG = "usb_file_mgr";
static httpd_handle_t s_server = NULL;
static bool s_usb_mounted = false;

static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; ++si) {
        if (src[si] == '%' && isxdigit((unsigned char)src[si + 1]) && isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = {src[si + 1], src[si + 2], '\0'};
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static esp_err_t get_query_param(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    char query[256] = {0};
    if (httpd_req_get_url_query_len(req) <= 0 || httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return ESP_FAIL;
    }

    char value[256] = {0};
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return ESP_FAIL;
    }

    url_decode(out, value, out_len);
    return ESP_OK;
}

static esp_err_t normalize_path(const char *in, char *out, size_t out_len)
{
    if (!in || in[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    if (strstr(in, "..")) {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(out, out_len, BASE_PATH "%s", in) >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t list_handler(httpd_req_t *req)
{
    if (!s_usb_mounted) {
        httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, "USB disk not mounted");
        return ESP_FAIL;
    }

    char rel_path[128] = "/";
    (void)get_query_param(req, "path", rel_path, sizeof(rel_path));

    char full_path[256] = {0};
    if (normalize_path(rel_path, full_path, sizeof(full_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    DIR *dir = opendir(full_path);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Path not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");

    bool first = true;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
        }

        char item_path[320];
        snprintf(item_path, sizeof(item_path), "%s/%s", full_path, ent->d_name);
        struct stat st = {0};
        stat(item_path, &st);

        char chunk[512];
        snprintf(chunk,
                 sizeof(chunk),
                 "%s{\"name\":\"%s\",\"is_dir\":%s,\"size\":%ld}",
                 first ? "" : ",",
                 ent->d_name,
                 S_ISDIR(st.st_mode) ? "true" : "false",
                 (long)st.st_size);
        httpd_resp_sendstr_chunk(req, chunk);
        first = false;
    }
    closedir(dir);

    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t download_handler(httpd_req_t *req)
{
    char rel_path[128] = {0};
    if (get_query_param(req, "path", rel_path, sizeof(rel_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
        return ESP_FAIL;
    }

    char full_path[256] = {0};
    if (normalize_path(rel_path, full_path, sizeof(full_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    FILE *fp = fopen(full_path, "rb");
    if (!fp) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char disp[192];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", strrchr(rel_path, '/') + 1);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char *buf = malloc(SCRATCH_BUFSIZE);
    if (!buf) {
        fclose(fp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    size_t nread;
    while ((nread = fread(buf, 1, SCRATCH_BUFSIZE, fp)) > 0) {
        if (httpd_resp_send_chunk(req, buf, nread) != ESP_OK) {
            free(buf);
            fclose(fp);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }

    free(buf);
    fclose(fp);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    char rel_path[128] = {0};
    if (get_query_param(req, "path", rel_path, sizeof(rel_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
        return ESP_FAIL;
    }

    char full_path[256] = {0};
    if (normalize_path(rel_path, full_path, sizeof(full_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    FILE *fp = fopen(full_path, "wb");
    if (!fp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    char *buf = malloc(SCRATCH_BUFSIZE);
    if (!buf) {
        fclose(fp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, remaining > SCRATCH_BUFSIZE ? SCRATCH_BUFSIZE : remaining);
        if (recv_len <= 0) {
            free(buf);
            fclose(fp);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
            return ESP_FAIL;
        }

        fwrite(buf, 1, recv_len, fp);
        remaining -= recv_len;
    }

    free(buf);
    fclose(fp);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    char rel_path[128] = {0};
    if (get_query_param(req, "path", rel_path, sizeof(rel_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
        return ESP_FAIL;
    }

    char full_path[256] = {0};
    if (normalize_path(rel_path, full_path, sizeof(full_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    if (unlink(full_path) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Delete failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset='utf-8'><title>ESP32S3 U盘管理</title>"
    "<style>body{font-family:sans-serif;max-width:760px;margin:20px auto;}button{margin-left:8px;}"
    "li{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #ddd}</style></head>"
    "<body><h2>ESP32-S3 U盘文件管理</h2><p id='status'>加载中...</p>"
    "<input id='file' type='file'/><button onclick='upload()'>上传到根目录</button><ul id='list'></ul>"
    "<script>async function refresh(){const r=await fetch('/api/list?path=/');"
    "if(!r.ok){document.getElementById('status').textContent='U盘未挂载或读取失败';return;}"
    "document.getElementById('status').textContent='已连接';const arr=await r.json();"
    "const ul=document.getElementById('list');ul.innerHTML='';arr.forEach(i=>{const li=document.createElement('li');"
    "li.innerHTML=`<span>${i.name} (${i.is_dir?'目录':i.size+'B'})</span>`;"
    "if(!i.is_dir){const a=document.createElement('a');a.href='/api/download?path=/'+encodeURIComponent(i.name);a.textContent='下载';"
    "const d=document.createElement('button');d.textContent='删除';d.onclick=async()=>{await fetch('/api/file?path=/'+encodeURIComponent(i.name),{method:'DELETE'});refresh();};"
    "const box=document.createElement('span');box.appendChild(a);box.appendChild(d);li.appendChild(box);}"
    "ul.appendChild(li);});}"
    "async function upload(){const f=document.getElementById('file').files[0];if(!f)return;"
    "await fetch('/api/upload?path=/'+encodeURIComponent(f.name),{method:'POST',body:await f.arrayBuffer()});refresh();}"
    "refresh();</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    httpd_uri_t list_uri = {.uri = "/api/list", .method = HTTP_GET, .handler = list_handler};
    httpd_uri_t download_uri = {.uri = "/api/download", .method = HTTP_GET, .handler = download_handler};
    httpd_uri_t upload_uri = {.uri = "/api/upload", .method = HTTP_POST, .handler = upload_handler};
    httpd_uri_t delete_uri = {.uri = "/api/file", .method = HTTP_DELETE, .handler = delete_handler};

    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &list_uri);
    httpd_register_uri_handler(s_server, &download_uri);
    httpd_register_uri_handler(s_server, &upload_uri);
    httpd_register_uri_handler(s_server, &delete_uri);
}

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL,
            .password = WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    if (strlen(WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started: SSID=%s PASS=%s", WIFI_SSID, WIFI_PASS);
}

static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static esp_err_t usb_disk_mount(void)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 20, NULL);

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = NULL,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_config));

    const msc_host_vfs_config_t vfs_config = {
        .base_path = BASE_PATH,
        .sector_size = 512,
    };

    esp_err_t err = msc_host_vfs_register(&vfs_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_vfs_register failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Waiting USB mass storage device...");
    msc_host_device_handle_t msc_device;
    err = msc_host_install_device(0, &msc_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No USB disk found: %s", esp_err_to_name(err));
        return err;
    }

    err = msc_host_vfs_mount(msc_device, BASE_PATH, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_usb_mounted = true;
    ESP_LOGI(TAG, "USB disk mounted at %s", BASE_PATH);
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_softap();

    if (usb_disk_mount() != ESP_OK) {
        ESP_LOGW(TAG, "USB disk not mounted, web server still runs");
    }

    start_web_server();
}
