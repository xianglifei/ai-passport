// main/app_wifi.h —— Wi-Fi 自动对时:开机连已存凭据 + SNTP;无凭据时 SoftAP 配网。
//
// 使用约定(参考 docs/reference/phoenixzhc/softap-provisioning-and-resource-budget):
//   * 所有 esp_wifi/httpd 操作必须在工作任务里调(本模块不自己建任务,
//     由 main 的 focus_work 队列分发);httpd 的 /save 回调在 httpd 任务里,
//     内部会自建一次性任务收尾,调用方无需关心。
//   * 状态与同步事件由 main 的 app_tick 轮询,不在 Wi-Fi 任务里碰 LVGL。
//   * 短时配网:max_connection=1,配网完成后完整停止 AP/HTTP。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_WIFI_IDLE = 0,       // 未运行(无凭据或已停止)
    APP_WIFI_CONNECTING,     // 正在连接路由器
    APP_WIFI_CONNECTED,      // 已拿到 IP,SNTP 对时中
    APP_WIFI_SYNCED,         // 时间已同步,Wi-Fi 已停止
    APP_WIFI_AP_MODE,        // 配网热点开启,等待手机接入
    APP_WIFI_AP_CONNECTED,   // 手机已连上热点(可打开页面)
    APP_WIFI_FAILED,         // 连接失败(超时/密码错误)
} app_wifi_state_t;

// 初始化 Wi-Fi 子系统并加载已存凭据。返回是否有凭据(有则调用方自动 start_sta)。
bool app_wifi_init(void);
bool app_wifi_has_creds(void);
const char *app_wifi_ap_name(void);
// 当前已存的 STA 名称(用于 UI 展示),无凭据返回空串。
const char *app_wifi_ssid(void);

app_wifi_state_t app_wifi_state(void);
// SNTP 同步事件(一次性):成功后经 *utc_sec 返回 UTC 秒。
bool app_wifi_poll_sync(uint64_t *utc_sec);
// 超时守护:CONNECTED 超 25s 视为失败、AP 开超 5 分钟自动收摊。app_tick 里调。
void app_wifi_tick(uint64_t now_ms);

// 以下三个在工作任务里调用。
bool app_wifi_start_sta(void);        // 连接已存凭据
bool app_wifi_start_provision(void);  // 开 SoftAP + 配网网页
bool app_wifi_finish_provision(void); // 收摊配网(AP/HTTP)并转入 STA 连接
bool app_wifi_stop(void);             // 停止 SNTP/Wi-Fi(同步完成后省电)
