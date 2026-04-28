/*
 * SPDX-FileCopyrightText: 2026 Your Name
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_dwin.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_vfs_fat.h"
#include "lauxlib.h"

static unsigned short *uni2oem_table = NULL;
static size_t uni2oem_count = 0;

static const char *TAG = "dwin";

/* ---------- 协议命令 ---------- */
enum {
  CMD_SEND_HEART = 0,
  CMD_ESP_READY = 1,
  CMD_SET_AI_TEXT = 10,
  CMD_SET_AI_EMOJI = 11,
  CMD_GET_HUMI_DATA = 20,
  CMD_GET_TEMP_DATA = 21,
  CMD_SET_PWM = 30,
  CMD_SET_RELAY = 31,
  CMD_GET_MQ2_VOL = 32,
  CMD_GET_MQ2_DO = 33,
  CMD_SET_LED = 40,
  CMD_SET_WIFI_SSID = 96,
  CMD_SET_WIFI_PSW = 97,
         CMD_SET_WIFI_CONNECT = 98,
  CMD_GET_WIFI_STATUS = 99,
  CMD_WIFI_STATUS_RESP = 100,
};

/* ---------- WiFi 状态码到中文文本映射 ---------- */
static const char *wifi_status_text[] = {
    "未得到用户名和密码", "WIFI路由器连接成功", "WIFI模块自升级中",
    "连接服务器中",       "已登录服务器",       "已连接云端"};
#define WIFI_STATUS_COUNT                                                      \
  (sizeof(wifi_status_text) / sizeof(wifi_status_text[0]))

/* ---------- 硬件配置 ---------- */
#define DWIN_UART_PORT UART_NUM_2
#define DWIN_TX_GPIO 1
#define DWIN_RX_GPIO 3
#define DWIN_BAUD_RATE 9600
#define DWIN_RX_BUF_SIZE 1024
#define DWIN_TX_BUF_SIZE 0
#define DWIN_TASK_STACK 3072
#define DWIN_TASK_PRIO 5
#define DWIN_FRAME_MAX_LEN 128
#define DWIN_TEXT_MAX_LEN 64

static bool g_initialized = false;
static TaskHandle_t g_task = NULL;
static SemaphoreHandle_t g_mutex = NULL;
static SemaphoreHandle_t g_sync_sem = NULL;

static float g_response_float = 0.0f;
static bool g_response_bool = false;
static int g_response_int = 0;
static bool g_response_arrived = false;
static int g_expected_cmd = -1;

/* ---------- UTF-8 / GBK 转换 ---------- */
static int utf8_to_unicode(const char **utf8) {
  const unsigned char *s = (const unsigned char *)(*utf8);
  if (s[0] < 0x80) {
    *utf8 += 1;
    return s[0];
  }
  if ((s[0] & 0xE0) == 0xC0) {
    *utf8 += 2;
    return ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
  }
  if ((s[0] & 0xF0) == 0xE0) {
    *utf8 += 3;
    return ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
  }
  *utf8 += 1;
  return 0xFFFD;
}

static bool uni2gbk(unsigned short unicode, unsigned short *gbk) {
  if (unicode < 0x80) {
    *gbk = unicode;
    return true;
  }
  int low = 0, high = (int)uni2oem_count - 1;
  while (low <= high) {
    int mid = (low + high) / 2;
    unsigned short u = uni2oem_table[mid * 2];
    if (u == unicode) {
      *gbk = uni2oem_table[mid * 2 + 1];
      return true;
    } else if (u < unicode) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  return false;
}

static bool load_uni2oem_table(void) {
  FILE *f = fopen("/fatfs/conv/uni2oem.bin", "rb");
  if (!f) {
    ESP_LOGE(TAG, "Cannot open /fatfs/conv/uni2oem.bin");
    return false;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  uni2oem_count = size / sizeof(unsigned short) / 2;
  if (uni2oem_count == 0) {
    fclose(f);
    return false;
  }

  uni2oem_table = (unsigned short *)malloc(size);
  if (!uni2oem_table) {
    fclose(f);
    ESP_LOGE(TAG, "OOM for uni2oem table");
    return false;
  }
  if (fread(uni2oem_table, 1, size, f) != (size_t)size) {
    fclose(f);
    free(uni2oem_table);
    uni2oem_table = NULL;
    return false;
  }
  fclose(f);
  ESP_LOGI(TAG, "Loaded %u entries from uni2oem.bin", (unsigned)uni2oem_count);
  return true;
}

static size_t utf8_to_gbk(const char *utf8, unsigned char *gbk_buf,
                          size_t buf_size) {
  size_t out = 0;
  while (*utf8 && out < buf_size) {
    unsigned short unicode = utf8_to_unicode(&utf8);
    unsigned short gbk;
    if (uni2gbk(unicode, &gbk)) {
      if (gbk < 0x80) {
        gbk_buf[out++] = (unsigned char)gbk;
      } else {
        if (out + 1 < buf_size) {
          gbk_buf[out++] = (unsigned char)(gbk >> 8);
          gbk_buf[out++] = (unsigned char)(gbk & 0xFF);
        } else
          break;
      }
    } else {
      gbk_buf[out++] = '?';
    }
  }
  return out;
}

/* ---------- 串口发送 ---------- */
static void dwin_send_raw(int cmd, const char *arg) {
  char buf[DWIN_FRAME_MAX_LEN];
  if (arg && strlen(arg) > 0) {
    snprintf(buf, sizeof(buf), "@%d,%s;\r\n", cmd, arg);
  } else {
    snprintf(buf, sizeof(buf), "@%d;\r\n", cmd);
  }
  uart_write_bytes(DWIN_UART_PORT, buf, strlen(buf));
  ESP_LOGD(TAG, "Sent: %s", buf);
}

static void dwin_send_cmd(int cmd) { dwin_send_raw(cmd, NULL); }
static void dwin_send_cmd_arg(int cmd, const char *arg) {
  dwin_send_raw(cmd, arg);
}

/* ---------- 等待回复 ---------- */
static bool dwin_wait_float(uint32_t timeout_ms, float *out) {
  TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
  if (xSemaphoreTake(g_sync_sem, ticks) == pdTRUE) {
    *out = g_response_float;
    return true;
  }
  return false;
}

static bool dwin_wait_bool(uint32_t timeout_ms, bool *out) {
  TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
  if (xSemaphoreTake(g_sync_sem, ticks) == pdTRUE) {
    *out = g_response_bool;
    return true;
  }
  return false;
}

static bool dwin_wait_int(uint32_t timeout_ms, int *out) {
  TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
  if (xSemaphoreTake(g_sync_sem, ticks) == pdTRUE) {
    *out = g_response_int;
    return true;
  }
  return false;
}

/* ---------- 后台串口解析任务 ---------- */
static void dwin_driver_task(void *arg) {
  uint8_t byte;
  char frame[DWIN_FRAME_MAX_LEN] = {0};
  size_t frame_len = 0;

  while (true) {
    int len = uart_read_bytes(DWIN_UART_PORT, &byte, 1, pdMS_TO_TICKS(10));
    if (len > 0) {
      if (byte == '@') {
        frame_len = 0;
      } else if (byte == ';') {
        frame[frame_len] = '\0';
        if (frame_len > 0) {
          char *comma = strchr(frame, ',');
          int cmd = -1;
          char *arg_str = NULL;

          if (comma) {
            *comma = '\0';
            cmd = atoi(frame);
            arg_str = comma + 1;
          } else {
            cmd = atoi(frame);
            arg_str = "";
          }

          ESP_LOGD(TAG, "Recv: cmd=%d arg=%s", cmd, arg_str);

          xSemaphoreTake(g_mutex, portMAX_DELAY);
          if (cmd == g_expected_cmd) {
            switch (cmd) {
            case CMD_GET_TEMP_DATA:
            case CMD_GET_HUMI_DATA:
            case CMD_GET_MQ2_VOL:
              g_response_float = strtof(arg_str, NULL);
              g_response_arrived = true;
              break;
            case CMD_GET_MQ2_DO:
              g_response_bool = (atoi(arg_str) != 0);
              g_response_arrived = true;
              break;
            case CMD_WIFI_STATUS_RESP:
              g_response_int = atoi(arg_str);
              g_response_arrived = true;
              break;
            default:
              break;
            }
            if (g_response_arrived) {
              g_expected_cmd = -1;
              xSemaphoreGive(g_sync_sem);
            }
          }
          xSemaphoreGive(g_mutex);
        }
      } else {
        if (frame_len < DWIN_FRAME_MAX_LEN - 1) {
          frame[frame_len++] = (char)byte;
        }
      }
    }
  }
}

/* ---------- API 实现 ---------- */
static void dwin_api_set_text(const char *utf8_text) {
  unsigned char gb_buf[128];
  size_t len = utf8_to_gbk(utf8_text, gb_buf, sizeof(gb_buf));
  gb_buf[len] = '\0';
  dwin_send_cmd_arg(CMD_SET_AI_TEXT, (const char *)gb_buf);
}

static void dwin_api_set_emoji(int id) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", id);
  dwin_send_cmd_arg(CMD_SET_AI_EMOJI, buf);
}

static bool dwin_api_get_temperature(float *temp, uint32_t timeout_ms) {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_expected_cmd = CMD_GET_TEMP_DATA;
  g_response_arrived = false;
  xSemaphoreGive(g_mutex);
  dwin_send_cmd(CMD_GET_TEMP_DATA);
  return dwin_wait_float(timeout_ms, temp);
}

static bool dwin_api_get_humidity(float *humi, uint32_t timeout_ms) {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_expected_cmd = CMD_GET_HUMI_DATA;
  g_response_arrived = false;
  xSemaphoreGive(g_mutex);
  dwin_send_cmd(CMD_GET_HUMI_DATA);
  return dwin_wait_float(timeout_ms, humi);
}

static bool dwin_api_get_mq2_voltage(float *vol, uint32_t timeout_ms) {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_expected_cmd = CMD_GET_MQ2_VOL;
  g_response_arrived = false;
  xSemaphoreGive(g_mutex);
  dwin_send_cmd(CMD_GET_MQ2_VOL);
  return dwin_wait_float(timeout_ms, vol);
}

static bool dwin_api_get_mq2_alarm(bool *alarm, uint32_t timeout_ms) {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_expected_cmd = CMD_GET_MQ2_DO;
  g_response_arrived = false;
  xSemaphoreGive(g_mutex);
  dwin_send_cmd(CMD_GET_MQ2_DO);
  return dwin_wait_bool(timeout_ms, alarm);
}

static void dwin_api_set_pwm(int duty) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", duty);
  dwin_send_cmd_arg(CMD_SET_PWM, buf);
}

static void dwin_api_set_relay(bool on) {
  dwin_send_cmd_arg(CMD_SET_RELAY, on ? "1" : "0");
}

/**
 * @brief 控制 LED
 * @param led 1=黄色, 2=绿色, 3=红色
 * @param on true 开, false 关
 */
static void dwin_api_set_led(int led, bool on) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d,%d", led, on ? 1 : 0);
  dwin_send_cmd_arg(CMD_SET_LED, buf);
}

static void dwin_api_set_wifi_ssid(const char *ssid) {
  dwin_send_cmd_arg(CMD_SET_WIFI_SSID, ssid);
}

static void dwin_api_set_wifi_psw(const char *psw) {
  dwin_send_cmd_arg(CMD_SET_WIFI_PSW, psw);
}

static void dwin_api_wifi_connect(void) {
  dwin_send_cmd_arg(CMD_SET_WIFI_CONNECT, "1");
}

static bool dwin_api_get_wifi_status(int *status, uint32_t timeout_ms) {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_expected_cmd = CMD_WIFI_STATUS_RESP;
  g_response_arrived = false;
  xSemaphoreGive(g_mutex);
  dwin_send_cmd(CMD_GET_WIFI_STATUS);
  return dwin_wait_int(timeout_ms, status);
}

static const char *dwin_api_get_wifi_status_text(void) {
  int status = 0;
  if (!dwin_api_get_wifi_status(&status, 5000)) {
    return "查询超时";
  }
  if (status >= 0 && status < (int)WIFI_STATUS_COUNT) {
    return wifi_status_text[status];
  }
  return "未知状态";
}

static void dwin_api_report_ready(void) { dwin_send_cmd(CMD_ESP_READY); }

/* ---------- Lua 绑定 ---------- */
static int l_dwin_set_text(lua_State *L) {
  const char *text = luaL_checkstring(L, 1);
  dwin_api_set_text(text);
  return 0;
}

static int l_dwin_set_emoji(lua_State *L) {
  int id = luaL_checkinteger(L, 1);
  dwin_api_set_emoji(id);
  return 0;
}

static int l_dwin_get_temperature(lua_State *L) {
  float temp;
  if (dwin_api_get_temperature(&temp, 2000)) {
    lua_pushnumber(L, temp);
    return 1;
  }
  lua_pushnil(L);
  lua_pushstring(L, "timeout");
  return 2;
}

static int l_dwin_get_humidity(lua_State *L) {
  float humi;
  if (dwin_api_get_humidity(&humi, 2000)) {
    lua_pushnumber(L, humi);
    return 1;
  }
  lua_pushnil(L);
  lua_pushstring(L, "timeout");
  return 2;
}

static int l_dwin_get_mq2_voltage(lua_State *L) {
  float vol;
  if (dwin_api_get_mq2_voltage(&vol, 2000)) {
    lua_pushnumber(L, vol);
    return 1;
  }
  lua_pushnil(L);
  lua_pushstring(L, "timeout");
  return 2;
}

static int l_dwin_get_mq2_alarm(lua_State *L) {
  bool alarm;
  if (dwin_api_get_mq2_alarm(&alarm, 2000)) {
    lua_pushboolean(L, alarm);
    return 1;
  }
  lua_pushnil(L);
  lua_pushstring(L, "timeout");
  return 2;
}

static int l_dwin_set_pwm(lua_State *L) {
  int duty = luaL_checkinteger(L, 1);
  dwin_api_set_pwm(duty);
  return 0;
}

static int l_dwin_set_relay(lua_State *L) {
  bool on = lua_toboolean(L, 1);
  dwin_api_set_relay(on);
  return 0;
}

static int l_dwin_set_led(lua_State *L) {
  int led = luaL_checkinteger(L, 1);
  bool on = lua_toboolean(L, 2);
  dwin_api_set_led(led, on);
  return 0;
}

static int l_dwin_set_wifi_ssid(lua_State *L) {
  const char *ssid = luaL_checkstring(L, 1);
  dwin_api_set_wifi_ssid(ssid);
  return 0;
}

static int l_dwin_set_wifi_psw(lua_State *L) {
  const char *psw = luaL_checkstring(L, 1);
  dwin_api_set_wifi_psw(psw);
  return 0;
}

static int l_dwin_wifi_connect(lua_State *L) {
  dwin_api_wifi_connect();
  return 0;
}

static int l_dwin_get_wifi_status(lua_State *L) {
  int status;
  if (dwin_api_get_wifi_status(&status, 5000)) {
    lua_pushinteger(L, status);
    return 1;
  }
  lua_pushnil(L);
  lua_pushstring(L, "timeout");
  return 2;
}

static int l_dwin_get_wifi_status_text(lua_State *L) {
  const char *text = dwin_api_get_wifi_status_text();
  lua_pushstring(L, text);
  return 1;
}

static int l_dwin_report_ready(lua_State *L) {
  dwin_api_report_ready();
  return 0;
}

static const luaL_Reg dwin_funcs[] = {
    {"set_text", l_dwin_set_text},
    {"set_emoji", l_dwin_set_emoji},
    {"get_temperature", l_dwin_get_temperature},
    {"get_humidity", l_dwin_get_humidity},
    {"get_mq2_voltage", l_dwin_get_mq2_voltage},
    {"get_mq2_alarm", l_dwin_get_mq2_alarm},
    {"set_pwm", l_dwin_set_pwm},
    {"set_relay", l_dwin_set_relay},
    {"set_led", l_dwin_set_led},
    {"set_wifi_ssid", l_dwin_set_wifi_ssid},
    {"set_wifi_psw", l_dwin_set_wifi_psw},
    {"wifi_connect", l_dwin_wifi_connect},
    {"get_wifi_status", l_dwin_get_wifi_status},
    {"get_wifi_status_text", l_dwin_get_wifi_status_text},
    {"report_ready", l_dwin_report_ready},
    {NULL, NULL}};

int luaopen_dwin(lua_State *L) {
  if (!g_initialized) {
    uart_config_t uart_cfg = {
        .baud_rate = DWIN_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(DWIN_UART_PORT, DWIN_RX_BUF_SIZE,
                                        DWIN_TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
      luaL_error(L, "dwin init failed (uart driver)");
    }

    err = uart_param_config(DWIN_UART_PORT, &uart_cfg);
    if (err != ESP_OK) {
      uart_driver_delete(DWIN_UART_PORT);
      ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
      luaL_error(L, "dwin init failed (config)");
    }

    err = uart_set_pin(DWIN_UART_PORT, DWIN_TX_GPIO, DWIN_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
      uart_driver_delete(DWIN_UART_PORT);
      ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
      luaL_error(L, "dwin init failed (pin)");
    }

    if (!load_uni2oem_table()) {
      luaL_error(L, "dwin: failed to load GBK table");
    }

    g_mutex = xSemaphoreCreateMutex();
    g_sync_sem = xSemaphoreCreateBinary();
    if (!g_mutex || !g_sync_sem) {
      uart_driver_delete(DWIN_UART_PORT);
      luaL_error(L, "dwin init failed (semaphore)");
    }

    xTaskCreate(dwin_driver_task, "dwin_task", DWIN_TASK_STACK, NULL,
                DWIN_TASK_PRIO, &g_task);
    g_initialized = true;
    ESP_LOGI(TAG, "Dwin driver initialized");
  }

  luaL_newlib(L, dwin_funcs);
  return 1;
}

esp_err_t lua_module_dwin_register(void) {
  return cap_lua_register_module("dwin", luaopen_dwin);
}