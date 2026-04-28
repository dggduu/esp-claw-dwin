# Lua Dwin Screen

This skill describes how to control a Dwin display, read its sensors,
and operate actuators (fan, LEDs, relay) from Lua. The module wraps the
low‑level serial protocol, performs automatic UTF‑8 to GBK conversion for
text, and provides synchronous, blocking calls for sensor queries.

## How to import
```lua
local dwin = require("dwin")
```

## Emoji mapping
The screen can show one of five emotion icons. Update both text and emoji
together for clear user feedback.

| ID | Constant    | Meaning                |
|----|-------------|------------------------|
| 0  | IDLE        | Default / standby      |
| 1  | LISTENING   | Waiting for input      |
| 2  | THINKING    | Processing             |
| 3  | SUCCESS     | Operation succeeded    |
| 4  | FAILED      | Operation failed       |

## Functions

### Display
- `dwin.set_text(utf8_str)` – Show Chinese or English text on the screen
  (converts UTF‑8 to GBK internally).
- `dwin.set_emoji(id)` – Change the emotion icon (0–4).

### Sensors (blocking)
Each sensor function blocks until data arrives or a 2‑second timeout fires.
On failure it returns `nil` and an error string.

- `dwin.get_temperature()` → temperature in °C (number)
- `dwin.get_humidity()` → relative humidity in % (number)
- `dwin.get_mq2_voltage()` → MQ‑2 analog voltage (number)
- `dwin.get_mq2_alarm()` → `true` if gas leak detected, otherwise `false`

### Actuators
- `dwin.set_pwm(duty)` – Set fan speed. `duty` is 0–100 (0 = off, 100 = full speed).
- `dwin.set_relay(on)` – Switch a general‑purpose relay (`true` / `false`).
- `dwin.set_led(led, on)` – Control one of three LEDs.
  - **`led`**: 
    - `1` – **Yellow** LED (warning / activity)
    - `2` – **Green** LED (success / normal)
    - `3` – **Red** LED (error / critical)
  - `on`: `true` (turn on) or `false` (turn off)

### WiFi (screen‑side)
- `dwin.set_wifi_ssid(ssid)`
- `dwin.set_wifi_psw(password)`
- `dwin.wifi_connect()`
- `dwin.get_wifi_status()` → integer status code:(0为连接失败，非0为连接正常)
  - `0`: 未得到用户名和密码
  - `1`: WIFI连接成功
  - `2`: WIFI模块自升级中
  - `3`: 连接服务器中
  - `4`: 已登录服务器
  - `5`: 已连接云端服务器 
- `dwin.get_wifi_status_text()` → human‑readable Chinese description of the
  current WiFi status (blocking, 5‑second timeout). Use this only if you
  need a textual description; otherwise prefer `get_wifi_status()`.

### System
- `dwin.report_ready()` – Tell the screen the ESP32 is ready.

## Important timing notes for WiFi connection
The screen processes serial commands sequentially. After calling
`dwin.wifi_connect()`, the screen may take several hundred milliseconds to
start the actual connection attempt. **Always wait at least 1 second** before
querying the status, otherwise you might read a stale status or cause a
timeout.

If your Lua environment does not provide `sys.wait()`, you can use a simple
busy loop:
```lua
local function sleep_ms(ms)
    local t = os.clock() * 1000
    while os.clock() * 1000 - t < ms do end
end
```

## Semantic examples

### Example 1 – Temperature control with LED colors
```lua
local dwin = require("dwin")

dwin.set_emoji(2)                 -- thinking
dwin.set_text("读取温度中...")

local temp = dwin.get_temperature()
if temp then
    dwin.set_text(string.format("当前温度：%.1f ℃", temp))
    if temp > 30 then
        dwin.set_pwm(80)          -- turn on fan
        dwin.set_led(1, true)     -- Yellow LED = warning
        dwin.set_text("过热！风扇已开启")
    elseif temp > 25 then
        dwin.set_led(2, true)     -- Green LED = normal
        dwin.set_led(1, false)
    else
        dwin.set_pwm(0)
        dwin.set_led(1, false)
        dwin.set_led(2, false)
        dwin.set_text("温度舒适，风扇关闭")
    end
    dwin.set_emoji(3)             -- success
else
    dwin.set_text("传感器无响应")
    dwin.set_emoji(4)             -- failed
end
```

### Example 2 – Gas leak alarm (red LED)
```lua
local dwin = require("dwin")

dwin.set_emoji(2)
dwin.set_text("检查燃气...")

local alarm = dwin.get_mq2_alarm()
if alarm == nil then
    dwin.set_text("MQ2 传感器故障")
    dwin.set_emoji(4)
elseif alarm then
    dwin.set_text("检测到燃气泄漏！")
    dwin.set_emoji(4)             -- warning
    dwin.set_led(3, true)         -- Red LED = critical
    dwin.set_led(1, true)         -- Yellow LED = caution
    dwin.set_pwm(100)             -- full fan speed
    dwin.set_relay(true)          -- close gas valve
else
    dwin.set_text("空气安全")
    dwin.set_emoji(0)
    dwin.set_led(1, false)
    dwin.set_led(3, false)
    dwin.set_pwm(0)
    dwin.set_relay(false)
end
```

### Example 3 – WiFi connection with progress and status text
```lua
local dwin = require("dwin")

-- Helper: ms sleep (adapt to your environment)
local function sleep(ms)
    local deadline = os.clock() + ms / 1000
    while os.clock() < deadline do end
end

dwin.set_wifi_ssid("MyHomeWiFi")
dwin.set_wifi_psw("secret123")
dwin.set_emoji(2)
dwin.set_led(1, true)             -- Yellow LED = activity
dwin.set_text("正在连接 WiFi...")
dwin.wifi_connect()

-- Wait for screen to process the connect command
sleep(1000)

local status = dwin.get_wifi_status()
dwin.set_led(1, false)

if status == 1 then
    dwin.set_led(2, true)         -- Green LED = connected
    dwin.set_text("WiFi 已连接")
    dwin.set_emoji(3)
elseif status == 0 then
    dwin.set_led(3, true)         -- Red LED = failure
    dwin.set_text("WiFi 连接失败（未获取到用户名/密码）")
    dwin.set_emoji(4)
elseif status == 2 or status == 3 then
    dwin.set_led(1, true)         -- Yellow LED for intermediate states
    dwin.set_text(dwin.get_wifi_status_text())
    dwin.set_emoji(2)
else
    dwin.set_text("未知状态: " .. tostring(status))
    dwin.set_emoji(0)
end
```

## Notes
- Sensor queries are blocking – the Lua task pauses until the reply arrives
  or the timeout expires. Avoid calling them in tight loops.
- The serial driver initialises automatically on first `require()`.
- Hardware: UART2, GPIO 1 (TX), GPIO 3 (RX), 9600 baud (change in source if needed).
- `set_pwm` expects a percentage; actual PWM is handled by the Dwin hardware.
- Always update the emoji together with the text for clear user feedback.
- **WiFi timing**: Always add a delay after `wifi_connect()` before reading
  status, otherwise you may get stale data or timeouts.