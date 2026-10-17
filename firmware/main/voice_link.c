#include "voice_link.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "bsp_board_extra.h"
#include "audio_out.h"
#include "flight_state.h"
#include "ui_ahrs.h"

/* PTT：按住 KEY(GPIO18) 录音，松开发送到 Mac 语音管线（WiFi TCP），收到回复 PCM 后播放。
   帧格式：magic(4B) + u32le 长度 + payload（16kHz 16bit 单声道裸 PCM）。TCP 可靠有序。
   WiFi 凭据和服务器地址在 voice_secrets.h（git 忽略，参考 voice_secrets.example.h） */
#include "voice_secrets.h"

#define PTT_GPIO        GPIO_NUM_18
#define WIFI_SSID       VOICE_WIFI_SSID
#define WIFI_PASS       VOICE_WIFI_PASS
#define SERVER_IP       VOICE_SERVER_IP
#define SERVER_PORT     7890

#define REC_MAX_SEC     6
#define SAMPLE_RATE     16000
#define REC_BUF_BYTES   (SAMPLE_RATE * 2 * REC_MAX_SEC)   /* 192KB PSRAM */
#define REPLY_BUF_BYTES (SAMPLE_RATE * 2 * 10)            /* 320KB PSRAM，10s 上限 */

static const char *TAG = "vlink";

static uint8_t *s_rec_buf, *s_reply_buf, *s_frame_buf;   /* s_frame_buf: 流式 chunk 组装（32KB PSRAM） */
static volatile int s_sock = -1;
static EventGroupHandle_t s_wifi_events;
#define GOT_IP_BIT BIT0

typedef enum {
    VS_IDLE = 0,
    VS_RECORDING,
    VS_PROCESSING,  /* 已发送，等 Mac 回复 */
    VS_SPEAKING,    /* 播报回复中 */
} voice_state_t;

static volatile voice_state_t s_state = VS_IDLE;
static volatile int64_t s_rx_beat;   /* rx 任务活动心跳（看门狗用） */

bool voice_link_active(void)
{
    return s_state != VS_IDLE;
}

static void set_state(voice_state_t st)
{
    s_state = st;
    ui_ahrs_set_voice_state((int)st);
}

static bool send_frame(const char *magic, const uint8_t *payload, uint32_t len)
{
    int fd = s_sock;
    if (fd < 0) {
        ESP_LOGW(TAG, "no server connection, frame dropped");
        return false;
    }
    uint8_t hdr[8];
    memcpy(hdr, magic, 4);
    hdr[4] = len & 0xFF;
    hdr[5] = (len >> 8) & 0xFF;
    hdr[6] = (len >> 16) & 0xFF;
    hdr[7] = (len >> 24) & 0xFF;
    if (send(fd, hdr, 8, 0) != 8) goto fail;
    size_t off = 0;
    while (off < len) {
        int w = send(fd, payload + off, len - off, 0);
        if (w <= 0) goto fail;
        off += (uint32_t)w;
    }
    return true;
fail:
    ESP_LOGW(TAG, "tx failed, frame dropped");
    close(fd);
    s_sock = -1;    /* rx 任务会重连 */
    return false;
}

/* 关键事件同时打到 USB 日志和 TCP（LOG0 帧），服务端可见 */
static void link_log(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) send_frame("LOG0", (const uint8_t *)buf, (uint32_t)n);
}

/* WiFi 事件 */
static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, GOT_IP_BIT);
        ESP_LOGW(TAG, "wifi disconnected, retry");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "wifi got ip: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, GOT_IP_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_events = xEventGroupCreate();
    esp_err_t nerr = esp_netif_init();   /* lwip tcpip 线程；缺失会导致 tcpip_inpkt Invalid mbox 断言 */
    if (nerr != ESP_OK && nerr != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(nerr));
    }
    if (esp_event_loop_create_default() != ESP_OK) {
        /* BSP 已建默认事件循环则忽略 */
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_handler, NULL, NULL);
    wifi_config_t wc = {0};
    strcpy((char *)wc.sta.ssid, WIFI_SSID);
    strcpy((char *)wc.sta.password, WIFI_PASS);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);   /* 关省电：实时性优先，防包延迟堆积 */
}

/* 流式播放：1s 预缓冲后随收随播；AUD1 空帧 = 流结束 */
static size_t s_sfill;
static bool s_speaking;
static bool s_started;

static void stream_flush(void)
{
    if (s_sfill) {
        /* 统一走 audio 任务队列写 codec（esp_codec_dev_write 非线程安全） */
        audio_stream_write(s_reply_buf, s_sfill);
    }
    s_sfill = 0;
}

static void stream_reset(void)
{
    s_sfill = 0;
    s_started = false;
}

static void stream_feed(const uint8_t *data, size_t len)
{
    if (s_sfill + len > REPLY_BUF_BYTES) stream_flush();
    memcpy(s_reply_buf + s_sfill, data, len);
    s_sfill += len;
    if (!s_started && s_sfill < 32768) return;  /* 1s 预缓冲后再开播 */
    if (!s_started) { ESP_LOGI(TAG, "play begin (prebuf %u B)", (unsigned)s_sfill); link_log("play begin %u B", (unsigned)s_sfill); }
    s_started = true;
    stream_flush();   /* 之后每来一块立即喂 DMA（write 阻塞即自然背压） */
}

/* 流结束：冲掉残余后**等播放队列排空**再灭绿点（队列里可能还压着几秒未播音频）。
   等待期间用户可按 KEY 抢话（状态被 ptt 任务切走就不再回 IDLE）。 */
static void stream_end(void)
{
    stream_flush();
    s_speaking = false;
    int64_t t0 = esp_timer_get_time();
    while (s_state == VS_SPEAKING && !audio_stream_idle() &&
           esp_timer_get_time() - t0 < 20 * 1000000) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(300));   /* codec DMA 尾巴 */
    if (s_state == VS_SPEAKING) set_state(VS_IDLE);
    ESP_LOGI(TAG, "stream done");
}
static void rx_task(void *arg)
{
    (void)arg;
    xEventGroupWaitBits(s_wifi_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    while (1) {
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (fd < 0) goto retry;
        {
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(SERVER_PORT);
            inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                ESP_LOGW(TAG, "connect %s:%d failed", SERVER_IP, SERVER_PORT);
                close(fd);
                goto retry;
            }
        }
        ESP_LOGI(TAG, "server connected");
        s_sock = fd;

        /* 帧解析：扫描 AUD1 + u32le 长度 + PCM（TCP 保序，magic 重同步兜底） */
        {
            enum { S_SCAN, S_HDR, S_DATA } rx = S_SCAN;
            int match = 0;
            uint32_t want = 0, got = 0;
            uint8_t b;
            int hdr_got = 0;
            uint32_t len_acc = 0;
            uint8_t rbuf[512];
            while (s_sock == fd) {
                s_rx_beat = esp_timer_get_time();
                int n = recv(fd, rbuf, sizeof(rbuf), 0);
                if (n <= 0) { link_log("rx exit n=%d", n); break; }
                for (int i = 0; i < n; i++) { b = rbuf[i];
                switch (rx) {
                case S_SCAN:
                    if ((match == 0 && b == 'A') || (match == 1 && b == 'U') ||
                        (match == 2 && b == 'D') || (match == 3 && b == '1')) {
                        match++;
                    } else if (match == 0 && b == 'P') {
                        match = 10;
                    } else if (match == 10 && b == 'I') {
                        match = 11;
                    } else if (match == 11 && b == 'N') {
                        match = 12;
                    } else if (match == 12 && b == 'G') {
                        link_log("ping");
                        match = 0;
                    } else {
                        match = (b == 'A') ? 1 : 0;
                    }
                    if (match == 4) {
                        match = 0;
                        rx = S_HDR;
                        hdr_got = 0;
                        len_acc = 0;
                    }
                    break;
                case S_HDR:
                    len_acc |= (uint32_t)b << (hdr_got * 8);
                    if (++hdr_got == 4) {
                        want = len_acc;
                        link_log("hdr want=%u", (unsigned)len_acc);
                        got = 0;
                        if (want == 0) {
                            /* 空帧 = 流结束：等播完回 idle，并重连（保活连接假死的根治） */
                            stream_end();
                            link_log("stream done, reconnect");
                            close(fd);
                            s_sock = -1;
                            break;  /* 退出内层 while，走外层重连 */
                        } else if (want <= 32768) {
                            rx = S_DATA;
                        } else {
                            rx = S_SCAN;
                        }
                    }
                    break;
                case S_DATA:
                    s_frame_buf[got++] = b;
                    if (got >= want) {
                        rx = S_SCAN;
                        if (want == 0) {
                            /* 流结束：等播完回 idle */
                            stream_end();
                        } else {
                            if (!s_speaking) {
                                stream_reset();
                                s_speaking = true;
                                set_state(VS_SPEAKING);
                                ESP_LOGI(TAG, "stream start, first chunk %u B", (unsigned)want);
                                link_log("stream start %u B", (unsigned)want);
                            }
                            stream_feed(s_frame_buf, want);
                        }
                    }
                    break;
                }
                }
            }
        }
        s_sock = -1;
        close(fd);
        ESP_LOGW(TAG, "server disconnected");
retry:
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* 录音 + 发送任务：轮询 PTT 按键 */
static void ptt_task(void *arg)
{
    (void)arg;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PTT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);

    bool held = false;
    size_t rec_len = 0;
    int64_t proc_since = 0;
    while (1) {
        int level = gpio_get_level(PTT_GPIO);
        if (!held && level == 0) {
            /* 按下：开始录音 */
            held = true;
            rec_len = 0;
            set_state(VS_RECORDING);
            ESP_LOGI(TAG, "PTT: recording");
            link_log("PTT recording");
        }
        if (held) {
            size_t got = 0;
            size_t room = REC_BUF_BYTES - rec_len;
            if (room > 0) {
                size_t want = room > 4096 ? 4096 : room;
                if (bsp_extra_i2s_read(s_rec_buf + rec_len, want, &got, 50) == ESP_OK) {
                    rec_len += got;
                }
            }
            if (level != 0 || rec_len >= REC_BUF_BYTES) {
                /* 松开（或录满）：发送 */
                held = false;
                if (rec_len > SAMPLE_RATE) {  /* 至少 0.5s 才发 */
                    ESP_LOGI(TAG, "PTT: send %u bytes", (unsigned)rec_len);
                    link_log("PTT send %u B", (unsigned)rec_len);
                    set_state(VS_PROCESSING);  /* 松手立刻变黄 */
                    proc_since = esp_timer_get_time();
                    /* 先发实时飞行数据（服务端须在音频帧之前拿到），再发语音 */
                    flight_state_t fs;
                    flight_state_snapshot(&fs);
                    char st[128];
                    int sn = snprintf(st, sizeof(st),
                                      "alt=%.0f,spd=%.0f,hdg=%.0f,pitch=%.0f,roll=%.0f,vs=%.1f",
                                      fs.altitude_m, fs.airspeed_kmh, fs.heading,
                                      fs.pitch, fs.roll, fs.vspeed_ms);
                    if (sn > 0) send_frame("STA0", (const uint8_t *)st, (uint32_t)sn);
                    if (send_frame("AUD0", s_rec_buf, (uint32_t)rec_len)) {
                        link_log("tx done");
                    } else {
                        set_state(VS_IDLE);
                    }
                } else {
                    set_state(VS_IDLE);
                }
            }
        } else {
            /* 处理中超时回退：10s 没等到回复就回 idle */
            if (s_state == VS_PROCESSING &&
                esp_timer_get_time() - proc_since > 10000000) {
                ESP_LOGW(TAG, "reply timeout, back to idle");
                link_log("reply timeout");
                set_state(VS_IDLE);
            }
            static int wd;
            if (++wd >= 150) {   /* ~3s 一次：报告 rx 任务心跳年龄 + socket 侦察 */
                wd = 0;
                uint8_t tmp[64];
                int pk = s_sock >= 0 ? recv(s_sock, tmp, sizeof(tmp), MSG_PEEK | MSG_DONTWAIT) : -2;
                int serr = -1; socklen_t sl = sizeof(serr);
                if (s_sock >= 0) getsockopt(s_sock, SOL_SOCKET, SO_ERROR, &serr, &sl);
                link_log("wd age=%dms st=%d fd=%d pk=%d err=%d",
                         (int)((esp_timer_get_time() - s_rx_beat) / 1000), (int)s_state,
                         s_sock, pk, serr);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

esp_err_t voice_link_init(void)
{
    s_rec_buf = heap_caps_malloc(REC_BUF_BYTES, MALLOC_CAP_SPIRAM);
    s_reply_buf = heap_caps_malloc(REPLY_BUF_BYTES, MALLOC_CAP_SPIRAM);
    s_frame_buf = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM);
    if (!s_rec_buf || !s_reply_buf || !s_frame_buf) {
        ESP_LOGE(TAG, "no PSRAM for voice buffers (free=%u, need=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)(REC_BUF_BYTES + REPLY_BUF_BYTES + 32768));
        return ESP_ERR_NO_MEM;
    }

    wifi_init();

    if (xTaskCreatePinnedToCore(ptt_task, "vlink_ptt", 3072, NULL, 4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(rx_task, "vlink_rx", 4096, NULL, 4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "voice link ready (PTT=GPIO18, wifi->" SERVER_IP ")");
    return ESP_OK;
}
