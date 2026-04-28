# Lua Dwin Screen

This skill describes how to control a Dwin display, read its sensors, and operate actuators (fan, LED light, relay) from Lua.  
The module wraps the underlying serial protocol and performs automatic UTF‑8 to GBK conversion when showing text.

## How to call
- Import it with `local dwin = require("dwin")`
- Call `dwin.set_text(utf8_str)` to show text (e.g. Chinese) on the screen. The text is automatically converted to GBK inside the module.
- Call `dwin.set_emoji(id)` to change the emotion icon. Valid `id` values are:
  `0` idle, `1` listening, `2` thinking, `3` success, `4` failed.
- Call `dwin.get_temperature()` to retrieve the air temperature in °C (blocking, up to ~2 s). Returns a number or `nil, err` on timeout.
- Call `dwin.get_humidity()` to retrieve relative humidity in % (blocking, up to ~2 s). Returns a number or `nil, err` on timeout.
- Call `dwin.get_mq2_voltage()` to read the raw analog voltage from the MQ‑2 gas sensor (blocking, up to ~2 s). Returns a number or `nil, err` on timeout.
- Call `dwin.get_mq2_alarm()` to read the digital gas‑leak alarm state (blocking, up to ~2 s). Returns `true`/`false` or `nil, err` on timeout.
- Call `dwin.set_pwm(duty)` to control the **fan speed**. `duty` is a percentage in the range 0–100 (0 = off, 100 = full speed). The command is sent immediately.
- Call `dwin.set_relay(on)` to switch a **general‑purpose relay**. `on` is a boolean (`true` → closed, `false` → open).
- Call `dwin.set_led(on)` to turn the **LED light** on (`true`) or off (`false`).
- Call `dwin.set_wifi_ssid(ssid)` to send a WiFi SSID string to the screen.
- Call `dwin.set_wifi_psw(password)` to send the WiFi password to the screen.
- Call `dwin.wifi_connect()` to trigger the screen to connect to the configured WiFi (the module automatically appends the required ‘1’ argument).
- Call `dwin.get_wifi_status()` to check the WiFi connection status from the screen (blocking, up to ~5 s). Returns an integer: `0` → disconnected, `1` → connected, `2` → failed. Returns `nil, err` on timeout.
- Call `dwin.report_ready()` to notify the screen that the ESP32 is ready.

## Text encoding
- `dwin.set_text` expects UTF‑8 input. The module internally converts the string to GBK before sending it over the serial link.
- You can pass plain Chinese text directly, e.g. `dwin.set_text("当前温度：25.0 ℃")`. No manual encoding is needed.
- Keep text length reasonable (the screen buffer is limited). Long strings will be truncated by the driver task.

## Example: Temperature‑based fan control with Chinese display
```lua
local dwin = require("dwin")

dwin.set_emoji(2)                        -- thinking
dwin.set_text("正在读取温度...")

local temp = dwin.get_temperature()
if temp then
    dwin.set_text(string.format("当前温度：%.1f ℃", temp))
    if temp > 30 then
        dwin.set_pwm(80)                 -- turn on fan at 80%
        dwin.set_text("过热！风扇已开启")
    else
        dwin.set_pwm(0)                  -- turn off fan
        dwin.set_text("温度舒适，风扇关闭")
    end
    dwin.set_emoji(3)                    -- success
else
    dwin.set_text("传感器无响应")
    dwin.set_emoji(4)                    -- failed
end
```
## Example: Gas alarm with light and fan
```lua
local dwin = require("dwin")

local alarm = dwin.get_mq2_alarm()
if alarm == nil then
    dwin.set_text("MQ2 传感器故障")
elseif alarm then
    dwin.set_text("检测到燃气泄漏！")
    dwin.set_emoji(4)
    dwin.set_led(true)                   -- warning LED on
    dwin.set_pwm(100)                    -- full fan speed to ventilate
    dwin.set_relay(true)                 -- close gas valve (relay)
else
    dwin.set_text("空气安全")
    dwin.set_emoji(0)
    dwin.set_led(false)
    dwin.set_pwm(0)
    dwin.set_relay(false)
end
```
## Example: WiFi setup
```lua
local dwin = require("dwin")

dwin.set_wifi_ssid("MyHomeWiFi")
dwin.set_wifi_psw("secret123")
dwin.wifi_connect()

-- wait a moment then check status
local status = dwin.get_wifi_status()
if status == 1 then
    dwin.set_text("WiFi 已连接")
    dwin.set_emoji(3)
elseif status == 2 then
    dwin.set_text("WiFi 连接失败")
    dwin.set_emoji(4)
else
    dwin.set_text("WiFi 连接中...")
    dwin.set_emoji(2)
end
```
Notes
All sensor queries are blocking – they will pause the Lua script until data arrives or the timeout expires. Use them carefully in time‑sensitive scripts.
The serial driver is automatically initialised on the first require("dwin") using UART2 (GPIO 17 TX, GPIO 18 RX, 115200 baud).
The set_pwm() duty parameter expects a percentage. The actual PWM frequency and resolution are handled by the Dwin hardware.
Emoji IDs are defined as: 0=idle, 1=listening, 2=thinking, 3=success, 4=failed.