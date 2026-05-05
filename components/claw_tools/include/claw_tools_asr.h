/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 配置 ASR 服务端信息
 *
 * @param base_url ASR 服务地址，例如 "http://192.168.1.100:8000"
 * @param timeout_ms HTTP 请求超时时间（毫秒）
 */
void claw_tools_asr_set_config(const char *base_url, int timeout_ms);

/**
 * @brief 解码 QQ 腾讯定制 SILK V3 音频文件为 WAV
 *
 * 处理格式：跳过可选的前导 0x02 字节，验证 "#!SILK_V3" magic，
 * 逐帧解码（每帧 2 字节 LE 长度前缀 + 载荷），输出 24kHz 16-bit mono WAV。
 *
 * @param silk_path 输入 SILK 文件路径（通常扩展名为 .amr）
 * @param wav_path  输出 WAV 文件路径
 * @return ESP_OK 成功，其他失败
 */
esp_err_t decode_silkv3_qq(const char *silk_path, const char *wav_path);

/**
 * @brief 上传音频文件到 ASR 服务并获取转写文本
 *
 * @param file_path 音频文件路径（WAV 格式，16kHz/24kHz mono）
 * @param out_text  输出文本缓冲区
 * @param out_size  缓冲区大小
 * @return ESP_OK 成功，其他失败
 */
esp_err_t claw_tools_asr_upload(const char *file_path, char *out_text,
                                size_t out_size);

/**
 * @brief 将本地音频文件转换为文本（自动检测格式并解码）
 *
 * 如果是 .amr/.silk 文件，自动调用 decode_silkv3_qq 解码为 WAV 再上传。
 * 否则直接上传。
 *
 * @param audio_file_path 本地音频文件路径
 * @param out_text 输出文本缓冲区
 * @param out_size 缓冲区大小
 * @return ESP_OK 成功，其他失败
 */
esp_err_t claw_tools_asr_transcribe(const char *audio_file_path, char *out_text,
                                    size_t out_size);
/**
 * @brief 将qq的silk文件转换为wav
 *
 * @param silk_path
 * @param wav_path
 * @return esp_err_t
 */
esp_err_t decode_silkv3_qq(const char *silk_path, const char *wav_path);

/**
 * @brief 从内存缓冲区转写语音（SILK/AMR/WAV）
 * @param audio_data  音频数据指针
 * @param audio_len   数据长度
 * @param format_hint 格式提示："silk", "amr", "wav" 或 NULL 自动判断
 * @param out_text    输出文本缓冲区
 * @param out_size    缓冲区大小
 * @return ESP_OK on success
 */
esp_err_t claw_tools_asr_transcribe_buffer(const uint8_t *audio_data,
                                           size_t audio_len,
                                           const char *format_hint,
                                           char *out_text, size_t out_size);
#ifdef __cplusplus
}
#endif
