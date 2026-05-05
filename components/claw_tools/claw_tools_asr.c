#include "claw_tools_asr.h"
#include "cJSON.h"
// #include "esp_crt_bundle.h"
#include "SKP_Silk_SDK_API.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "claw_tools_asr";

static char s_asr_base_url[128] = "http://10.44.8.106:8000";
static int s_asr_timeout_ms = 10000;

void claw_tools_asr_set_config(const char *base_url, int timeout_ms) {
  if (base_url) {
    strlcpy(s_asr_base_url, base_url, sizeof(s_asr_base_url));
  }
  if (timeout_ms > 0) {
    s_asr_timeout_ms = timeout_ms;
  }
  ESP_LOGI(TAG, "ASR config: url=%s, timeout=%dms", s_asr_base_url,
           s_asr_timeout_ms);
}

/* ---------- WAV header ---------- */
#define SILK_MAGIC "#!SILK_V3"
#define SILK_MAGIC_LEN 9
#define SILK_SDK_MAX_FRAME_BYTES (4096)
#define SILK_SDK_OUT_BUF_SAMPLES (960 * 5) /* max: 20ms * 48kHz * 5 frames */

static esp_err_t write_wav_header_at(FILE *f, int sample_rate, int channels,
                                     int bits_per_sample, int data_bytes) {
  int byte_rate = sample_rate * channels * bits_per_sample / 8;
  int block_align = channels * bits_per_sample / 8;
  int chunk_size = 36 + data_bytes;

  rewind(f);
  fwrite("RIFF", 1, 4, f);
  fwrite(&chunk_size, 4, 1, f);
  fwrite("WAVE", 1, 4, f);
  fwrite("fmt ", 1, 4, f);
  int fmt_chunk_size = 16;
  short audio_format = 1;
  fwrite(&fmt_chunk_size, 4, 1, f);
  fwrite(&audio_format, 2, 1, f);
  fwrite(&channels, 2, 1, f);
  fwrite(&sample_rate, 4, 1, f);
  fwrite(&byte_rate, 4, 1, f);
  fwrite(&block_align, 2, 1, f);
  fwrite(&bits_per_sample, 2, 1, f);
  fwrite("data", 1, 4, f);
  fwrite(&data_bytes, 4, 1, f);
  return ESP_OK;
}

/* ---------- SILK V3 解码（QQ 腾讯定制格式，基于 silk-v3-decoder）---------- */

esp_err_t decode_silkv3_qq(const char *silk_path, const char *wav_path) {
  if (!silk_path || !wav_path) {
    ESP_LOGE(TAG, "decode_silkv3_qq: invalid arguments (NULL path)");
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "decode_silkv3_qq: input=%s, output=%s", silk_path, wav_path);

  /* 0. 确认输出目录存在 */
  char dir_part[280];
  strlcpy(dir_part, wav_path, sizeof(dir_part));
  char *last_slash = strrchr(dir_part, '/');
  if (last_slash) {
    *last_slash = '\0';
    if (mkdir(dir_part, 0777) != 0 && errno != EEXIST) {
      ESP_LOGE(TAG, "Failed to create output directory %s (errno=%d)", dir_part,
               errno);
      return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Output directory ensured: %s", dir_part);
  }

  FILE *fin = fopen(silk_path, "rb");
  if (!fin) {
    ESP_LOGE(TAG, "Cannot open input file: %s (errno=%d)", silk_path, errno);
    return ESP_ERR_NOT_FOUND;
  }
  ESP_LOGI(TAG, "Input file opened successfully");

  /* 1. 检测并跳过腾讯定制头部（0x02），验证 SILK magic */
  {
    char header_buf[16];
    int first_byte = fgetc(fin);
    ESP_LOGI(TAG, "First byte of file: 0x%02X", first_byte);

    if (first_byte == 0x02) {
      ESP_LOGI(TAG, "Detected Tencent silk header (0x02), skipping");
      if (fread(header_buf, 1, SILK_MAGIC_LEN, fin) != SILK_MAGIC_LEN ||
          memcmp(header_buf, SILK_MAGIC, SILK_MAGIC_LEN) != 0) {
        ESP_LOGE(TAG, "Not a SILK V3 file (bad magic after 0x02)");
        fclose(fin);
        return ESP_FAIL;
      }
    } else if (first_byte == '!') {
      header_buf[0] = '!';
      if (fread(header_buf + 1, 1, 7, fin) != 7 ||
          memcmp(header_buf, "!SILK_V3", 8) != 0) {
        ESP_LOGE(TAG, "Not a SILK V3 file (bad magic from '!')");
        fclose(fin);
        return ESP_FAIL;
      }
    } else {
      ESP_LOGE(TAG, "Unknown silk format, first byte=0x%02X", first_byte);
      fclose(fin);
      return ESP_FAIL;
    }
  }
  ESP_LOGI(TAG, "SILK V3 magic verified successfully");

  /* 2. 初始化解码器 */
  SKP_int32 dec_size;
  if (SKP_Silk_SDK_Get_Decoder_Size(&dec_size) != 0) {
    ESP_LOGE(TAG, "SKP_Silk_SDK_Get_Decoder_Size failed");
    fclose(fin);
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Decoder size: %ld bytes", (long)dec_size);

  void *dec_state = malloc(dec_size);
  if (!dec_state) {
    ESP_LOGE(TAG, "Failed to allocate decoder state (%ld bytes)",
             (long)dec_size);
    fclose(fin);
    return ESP_ERR_NO_MEM;
  }

  if (SKP_Silk_SDK_InitDecoder(dec_state) != 0) {
    ESP_LOGE(TAG, "SKP_Silk_SDK_InitDecoder failed");
    free(dec_state);
    fclose(fin);
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Decoder initialized successfully");

  SKP_SILK_SDK_DecControlStruct dec_ctrl = {0};
  dec_ctrl.API_sampleRate = 24000;
  dec_ctrl.framesPerPacket = 1;

  /* 3. 打开输出 WAV，先占位 header */
  FILE *fout = fopen(wav_path, "wb");
  if (!fout) {
    ESP_LOGE(TAG, "Cannot create output WAV file: %s (errno=%d)", wav_path,
             errno);
    free(dec_state);
    fclose(fin);
    return ESP_ERR_NOT_FOUND;
  }
  ESP_LOGI(TAG, "Output WAV file opened for writing");

  uint8_t placeholder[44] = {0};
  fwrite(placeholder, 1, sizeof(placeholder), fout);

  /* 4. 逐帧解码 */
  SKP_uint8 payload[SILK_SDK_MAX_FRAME_BYTES];
  SKP_int16 out_buf[SILK_SDK_OUT_BUF_SAMPLES];
  int total_pcm_bytes = 0;
  int packet_count = 0;

  while (1) {
    SKP_int16 nBytes = 0;
    size_t rd = fread(&nBytes, 1, sizeof(nBytes), fin);
    if (rd < sizeof(nBytes) || nBytes <= 0) {
      ESP_LOGI(TAG, "End of input or invalid frame length (nBytes=%d, rd=%u)",
               (int)nBytes, (unsigned)rd);
      break;
    }

    if (nBytes > SILK_SDK_MAX_FRAME_BYTES ||
        nBytes > (SKP_int16)sizeof(payload)) {
      ESP_LOGW(TAG, "Frame %d too large: %d bytes, truncating to %u",
               packet_count, (int)nBytes, (unsigned)sizeof(payload));
      nBytes = sizeof(payload);
    }

    if (fread(payload, 1, nBytes, fin) != (size_t)nBytes) {
      ESP_LOGW(TAG, "Truncated payload at packet %d (expected %d bytes)",
               packet_count, (int)nBytes);
      break;
    }

    SKP_int16 *out_ptr = out_buf;
    SKP_int16 tot_len = 0;
    int sub_frame = 0;
    do {
      SKP_int16 len = 0;
      SKP_int ret = SKP_Silk_SDK_Decode(dec_state, &dec_ctrl, 0, payload,
                                        nBytes, out_ptr, &len);
      if (ret != 0) {
        ESP_LOGE(TAG, "Decode error %d at packet %d, sub-frame %d", ret,
                 packet_count, sub_frame);
        if (packet_count == 0) {
          // first frame failed -> abort
          ESP_LOGE(TAG, "First packet decode failed, aborting");
          free(dec_state);
          fclose(fout);
          fclose(fin);
          remove(wav_path);
          return ESP_FAIL;
        }
        // subsequent errors: stop decoding this packet
        break;
      }
      out_ptr += len;
      tot_len += len;
      sub_frame++;
    } while (dec_ctrl.moreInternalDecoderFrames);

    if (tot_len > 0) {
      fwrite(out_buf, sizeof(SKP_int16), tot_len, fout);
      total_pcm_bytes += tot_len * sizeof(SKP_int16);
      ESP_LOGI(TAG, "Packet %d decoded: %d samples (%d bytes)", packet_count,
               tot_len, tot_len * (int)sizeof(SKP_int16));
    } else {
      ESP_LOGW(TAG, "Packet %d produced no PCM samples", packet_count);
    }
    packet_count++;
  }

  free(dec_state);
  fclose(fin);

  if (total_pcm_bytes == 0) {
    ESP_LOGE(TAG, "No PCM data decoded (0 bytes)");
    fclose(fout);
    remove(wav_path);
    return ESP_FAIL;
  }

  /* 5. 回填 WAV header */
  ESP_LOGI(TAG, "Writing WAV header: %d Hz, 1ch, 16bit, %d data bytes",
           dec_ctrl.API_sampleRate, total_pcm_bytes);
  write_wav_header_at(fout, dec_ctrl.API_sampleRate, 1, 16, total_pcm_bytes);
  fclose(fout);

  ESP_LOGI(TAG, "SILK->WAV complete: %s, %d packets, %d PCM bytes", wav_path,
           packet_count, total_pcm_bytes);
  return ESP_OK;
}

/* ---------- MIME ---------- */
static const char *get_mime_type(const char *path) {
  const char *ext = strrchr(path, '.');
  if (!ext)
    return "application/octet-stream";
  if (strcasecmp(ext, ".amr") == 0)
    return "audio/amr";
  if (strcasecmp(ext, ".wav") == 0)
    return "audio/wav";
  return "application/octet-stream";
}

/* ---------- HTTP 响应收集 ---------- */
typedef struct {
  char *data;
  size_t len;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  http_response_t *resp = (http_response_t *)evt->user_data;
  if (!resp)
    return ESP_OK;

  switch (evt->event_id) {
  case HTTP_EVENT_ON_DATA:
    if (evt->data_len > 0) {
      char *newdata = realloc(resp->data, resp->len + evt->data_len + 1);
      if (!newdata)
        return ESP_ERR_NO_MEM;
      resp->data = newdata;
      memcpy(resp->data + resp->len, evt->data, evt->data_len);
      resp->len += evt->data_len;
      resp->data[resp->len] = '\0';
    }
    break;
  default:
    break;
  }
  return ESP_OK;
}

/* ---------- 上传音频到 ASR 服务 ---------- */
esp_err_t claw_tools_asr_upload(const char *file_path, char *out_text,
                                size_t out_size) {
  struct stat st;
  if (stat(file_path, &st) != 0 || st.st_size <= 0) {
    ESP_LOGE(TAG, "File not found or empty: %s", file_path);
    return ESP_ERR_NOT_FOUND;
  }

  FILE *fp = fopen(file_path, "rb");
  if (!fp)
    return ESP_ERR_NOT_FOUND;
  uint8_t *file_data = malloc(st.st_size);
  if (!file_data) {
    fclose(fp);
    return ESP_ERR_NO_MEM;
  }
  size_t read_bytes = fread(file_data, 1, st.st_size, fp);
  fclose(fp);
  if (read_bytes != (size_t)st.st_size) {
    free(file_data);
    return ESP_FAIL;
  }

  const char *filename = strrchr(file_path, '/');
  filename = filename ? filename + 1 : file_path;
  const char *mime = get_mime_type(file_path);

  const char *boundary = "----ESP32ClawAsrBoundary";
  char *header_part = NULL;
  int header_len = asprintf(
      &header_part,
      "--%s\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
      "Content-Type: %s\r\n\r\n",
      boundary, filename, mime);
  if (header_len < 0) {
    free(file_data);
    return ESP_ERR_NO_MEM;
  }

  int body_len = header_len + st.st_size + strlen("\r\n--") + strlen(boundary) +
                 strlen("--\r\n") + 1;
  char *body = malloc(body_len);
  if (!body) {
    free(header_part);
    free(file_data);
    return ESP_ERR_NO_MEM;
  }

  char *ptr = body;
  memcpy(ptr, header_part, header_len);
  ptr += header_len;
  memcpy(ptr, file_data, st.st_size);
  ptr += st.st_size;
  ptr += sprintf(ptr, "\r\n--%s--\r\n", boundary);
  free(header_part);
  free(file_data);

  char url[256];
  snprintf(url, sizeof(url), "%s/v1/audio/transcriptions", s_asr_base_url);

  esp_http_client_config_t http_cfg = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = s_asr_timeout_ms,
      .event_handler = http_event_handler,
      .buffer_size = 4096,
      .buffer_size_tx = 2048,
      // .crt_bundle_attach = esp_crt_bundle_attach,
  };
  http_response_t resp = {0};
  http_cfg.user_data = &resp;

  esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
  if (!client) {
    free(body);
    return ESP_FAIL;
  }

  char content_type[128];
  snprintf(content_type, sizeof(content_type),
           "multipart/form-data; boundary=%s", boundary);
  esp_http_client_set_header(client, "Content-Type", content_type);
  esp_http_client_set_post_field(client, body, body_len);

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  free(body);

  if (err != ESP_OK || status != 200) {
    ESP_LOGE(TAG, "ASR HTTP failed: err=%d, status=%d, resp=%s", err, status,
             resp.data ? resp.data : "");
    free(resp.data);
    return ESP_FAIL;
  }

  cJSON *root = cJSON_Parse(resp.data);
  free(resp.data);
  if (!root) {
    ESP_LOGE(TAG, "ASR response JSON parse failed");
    return ESP_FAIL;
  }
  cJSON *text_json = cJSON_GetObjectItem(root, "text");
  if (!cJSON_IsString(text_json) || !text_json->valuestring) {
    cJSON_Delete(root);
    return ESP_FAIL;
  }
  strlcpy(out_text, text_json->valuestring, out_size);
  cJSON_Delete(root);
  ESP_LOGI(TAG, "ASR result: %s", out_text);
  return ESP_OK;
}

/* ---------- 对外接口：解码 + 转写 ---------- */
esp_err_t claw_tools_asr_transcribe(const char *audio_file_path, char *out_text,
                                    size_t out_size) {
  if (!audio_file_path || !out_text || out_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  bool is_silk = (strstr(audio_file_path, ".amr") != NULL);
  char temp_wav[128] = {0};
  const char *upload_path = audio_file_path;

  if (is_silk) {
    mkdir("/fatfs/tmp", 0777);
    snprintf(temp_wav, sizeof(temp_wav), "/fatfs/tmp/asr_%lld.wav",
             esp_timer_get_time());
    if (decode_silkv3_qq(audio_file_path, temp_wav) != ESP_OK) {
      ESP_LOGE(TAG, "SILK decode failed");
      return ESP_FAIL;
    }
    upload_path = temp_wav;
  }

  esp_err_t ret = claw_tools_asr_upload(upload_path, out_text, out_size);

  if (is_silk) {
    remove(temp_wav);
  }
  return ret;
}

esp_err_t claw_tools_asr_transcribe_buffer(const uint8_t *audio_data,
                                           size_t audio_len,
                                           const char *format_hint,
                                           char *out_text, size_t out_size) {
  if (!audio_data || audio_len == 0 || !out_text || out_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  // 确保临时目录存在
  mkdir("/fatfs/tmp", 0777);

  // 生成唯一临时文件名
  char temp_raw[128];
  char temp_wav[128];
  snprintf(temp_raw, sizeof(temp_raw), "/fatfs/tmp/asr_raw_%lld",
           esp_timer_get_time());
  snprintf(temp_wav, sizeof(temp_wav), "/fatfs/tmp/asr_%lld.wav",
           esp_timer_get_time());

  // 1. 写入原始数据到临时文件
  FILE *f = fopen(temp_raw, "wb");
  if (!f) {
    ESP_LOGE(TAG, "Cannot create temp file %s", temp_raw);
    return ESP_ERR_NO_MEM;
  }
  fwrite(audio_data, 1, audio_len, f);
  fclose(f);

  bool need_decode = (format_hint && strcmp(format_hint, "silk") == 0) ||
                     (audio_len > 8 && memcmp(audio_data, "\x02#!SILK", 6) ==
                                           0); // 简易 magic 检测
  const char *upload_path = temp_raw;

  if (need_decode) {
    // 2. SILK -> WAV
    esp_err_t ret = decode_silkv3_qq(temp_raw, temp_wav);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "SILK decode failed from buffer");
      remove(temp_raw);
      return ret;
    }
    upload_path = temp_wav;
  }

  // 3. 上传并转写
  esp_err_t ret = claw_tools_asr_upload(upload_path, out_text, out_size);

  // 4. 清理临时文件
  remove(temp_raw);
  if (need_decode) {
    remove(temp_wav);
  }

  return ret;
}