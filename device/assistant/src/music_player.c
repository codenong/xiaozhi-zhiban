/**
 * @file music_player.c
 * @brief 轻量流式 MP3 音乐播放器（HTTP/HTTPS + minimp3 + audio_track）
 *
 * 特性：
 * - 纯 C 定点 minimp3 解码，零外部进程开销
 * - 支持 http:// 与 https://（基于 tls_transport），支持 301/302 重定向与 chunked 传输
 * - 支持 Range 断点续播（pause / resume）
 * - 支持 defer_start（TTS 播报结束后自动起播）与唤醒打断自动暂停
 */

#include "music_player.h"
#include "tls_transport.h"
#include "reverse/audio_service_api.h"
#include "xiaozhi_config.h"
#include "plog.h"

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>

#define MP_URL_MAX          1536
#define MP_HOST_MAX         256
#define MP_PATH_MAX         1280
#define MP_TITLE_MAX        256
#define MP_MAX_REDIRECTS    5
#define MP_IN_BUF_SIZE      (32 * 1024)
#define MP_READ_CHUNK_SIZE  (8 * 1024)
#define MP_S32_BUF_SAMPLES  1152

typedef struct {
    bool use_ssl;
    tls_transport_t tls;
    int sockfd;
    bool connected;
} mp_conn_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_running;
    bool initialized;

    music_state_t state;
    bool stop_req;
    bool pause_req;
    bool defer_start;

    char url[MP_URL_MAX];
    char title[MP_TITLE_MAX];
    uint64_t resume_bytes;
    uint64_t played_ms;
    int sample_rate;
} mp_ctx_t;

static mp_ctx_t g_mp;

/* ---- URL 解析 ---- */

static int mp_parse_url(const char *url, bool *use_ssl, char *host, size_t host_sz,
                        int *port, char *path, size_t path_sz)
{
    if (!url || !use_ssl || !host || !port || !path)
        return -1;

    const char *p = url;
    if (strncasecmp(p, "https://", 8) == 0) {
        *use_ssl = true;
        *port = 443;
        p += 8;
    } else if (strncasecmp(p, "http://", 7) == 0) {
        *use_ssl = false;
        *port = 80;
        p += 7;
    } else {
        return -1;
    }

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen == 0 || hlen >= host_sz)
            return -1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535)
            return -1;
        p = slash ? slash : (p + strlen(p));
    } else if (slash) {
        size_t hlen = (size_t)(slash - p);
        if (hlen == 0 || hlen >= host_sz)
            return -1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        p = slash;
    } else {
        if (strlen(p) == 0 || strlen(p) >= host_sz)
            return -1;
        strcpy(host, p);
        p += strlen(p);
    }

    if (*p) {
        if (strlen(p) >= path_sz)
            return -1;
        strcpy(path, p);
    } else {
        strcpy(path, "/");
    }
    return 0;
}

static void mp_resolve_redirect(const char *base_url, const char *location, char *out, size_t out_sz)
{
    if (strncasecmp(location, "http://", 7) == 0 || strncasecmp(location, "https://", 8) == 0) {
        snprintf(out, out_sz, "%s", location);
        return;
    }
    bool ssl = false;
    char host[MP_HOST_MAX];
    int port = 80;
    char path[MP_PATH_MAX];
    if (mp_parse_url(base_url, &ssl, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        snprintf(out, out_sz, "%s", location);
        return;
    }
    const char *scheme = ssl ? "https" : "http";
    if ((ssl && port == 443) || (!ssl && port == 80)) {
        snprintf(out, out_sz, "%s://%s%s%s", scheme, host,
                 location[0] == '/' ? "" : "/", location);
    } else {
        snprintf(out, out_sz, "%s://%s:%d%s%s", scheme, host, port,
                 location[0] == '/' ? "" : "/", location);
    }
}

/* ---- HTTP/HTTPS 统一连接封装 ---- */

static int plain_tcp_connect(const char *host, int port)
{
    struct addrinfo hints, *res = NULL, *rp = NULL;
    char port_str[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return -1;

    int sock = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0)
            continue;

        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        int r = connect(sock, rp->ai_addr, rp->ai_addrlen);
        if (r == 0) {
            fcntl(sock, F_SETFL, flags);
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = sock, .events = POLLOUT };
            int pr = poll(&pfd, 1, 8000);
            if (pr > 0) {
                int so_err = 0;
                socklen_t slen = sizeof(so_err);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &slen) == 0 && so_err == 0) {
                    fcntl(sock, F_SETFL, flags);
                    break;
                }
            }
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);

    if (sock >= 0) {
        struct timeval tv = { .tv_sec = 8, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    return sock;
}

static int mp_conn_open(mp_conn_t *c, bool use_ssl, const char *host, int port)
{
    memset(c, 0, sizeof(*c));
    c->use_ssl = use_ssl;
    c->sockfd = -1;

    if (use_ssl) {
        if (tls_transport_init(&c->tls) != 0)
            return -1;
        if (tls_transport_connect(&c->tls, host, port) != 0) {
            tls_transport_destroy(&c->tls);
            return -1;
        }
        c->connected = true;
        return 0;
    } else {
        c->sockfd = plain_tcp_connect(host, port);
        if (c->sockfd < 0)
            return -1;
        c->connected = true;
        return 0;
    }
}

static void mp_conn_close(mp_conn_t *c)
{
    if (!c->connected)
        return;
    if (c->use_ssl) {
        tls_transport_disconnect(&c->tls);
        tls_transport_destroy(&c->tls);
    } else if (c->sockfd >= 0) {
        close(c->sockfd);
        c->sockfd = -1;
    }
    c->connected = false;
}

static int mp_conn_write(mp_conn_t *c, const void *buf, size_t len)
{
    if (!c->connected)
        return -1;
    if (c->use_ssl) {
        return tls_transport_write(&c->tls, (const uint8_t *)buf, len);
    }
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(c->sockfd, (const char *)buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return (int)sent;
}

static int mp_conn_read(mp_conn_t *c, uint8_t *buf, size_t max_len)
{
    if (!c->connected)
        return -1;
    if (c->use_ssl) {
        int pr = tls_transport_poll(&c->tls, 300);
        if (pr < 0)
            return -1;
        if (pr == 0)
            return -2; /* timeout */
        int r = tls_transport_read(&c->tls, buf, max_len);
        if (r == -2)
            return -2;
        return r;
    } else {
        struct pollfd pfd = { .fd = c->sockfd, .events = POLLIN };
        int pr = poll(&pfd, 1, 300);
        if (pr < 0)
            return (errno == EINTR) ? -2 : -1;
        if (pr == 0)
            return -2;
        ssize_t r = recv(c->sockfd, buf, max_len, 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return -2;
            return -1;
        }
        return (int)r;
    }
}

/* ---- HTTP 响应头与流式读取 ---- */

typedef struct {
    mp_conn_t conn;
    int status_code;
    bool chunked;
    int64_t content_length;
    int64_t body_read;
    int64_t chunk_rem;
    bool chunk_eof;
    char location[MP_URL_MAX];

    uint8_t pre_buf[4096];
    size_t pre_len;
    size_t pre_pos;
} mp_http_stream_t;

static int parse_http_headers(const char *hdr, mp_http_stream_t *st)
{
    if (sscanf(hdr, "HTTP/%*d.%*d %d", &st->status_code) != 1)
        return -1;

    st->content_length = -1;
    st->chunked = false;
    st->location[0] = '\0';

    const char *line = hdr;
    while (line && *line) {
        const char *eol = strstr(line, "\r\n");
        size_t llen = eol ? (size_t)(eol - line) : strlen(line);
        if (llen > 9 && strncasecmp(line, "Location:", 9) == 0) {
            const char *v = line + 9;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t vlen = (size_t)(line + llen - v);
            if (vlen >= sizeof(st->location))
                vlen = sizeof(st->location) - 1;
            memcpy(st->location, v, vlen);
            st->location[vlen] = '\0';
        } else if (llen > 15 && strncasecmp(line, "Content-Length:", 15) == 0) {
            st->content_length = strtoll(line + 15, NULL, 10);
        } else if (llen > 18 && strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            if (strncasecmp(line + 18, " chunked", 8) == 0 || strcasestr(line, "chunked"))
                st->chunked = true;
        }
        if (!eol)
            break;
        line = eol + 2;
    }
    return 0;
}

static bool should_abort_now(void)
{
    bool ab = false;
    pthread_mutex_lock(&g_mp.lock);
    ab = g_mp.stop_req || g_mp.pause_req;
    pthread_mutex_unlock(&g_mp.lock);
    return ab;
}

static int mp_stream_open(mp_http_stream_t *st, const char *initial_url, uint64_t range_offset)
{
    char cur_url[MP_URL_MAX];
    snprintf(cur_url, sizeof(cur_url), "%s", initial_url);

    for (int hop = 0; hop <= MP_MAX_REDIRECTS; hop++) {
        if (should_abort_now())
            return -1;

        bool use_ssl = false;
        char host[MP_HOST_MAX];
        int port = 80;
        char path[MP_PATH_MAX];
        if (mp_parse_url(cur_url, &use_ssl, host, sizeof(host), &port, path, sizeof(path)) != 0) {
            PLOG_E("MUSIC", "URL 解析失败: %s", cur_url);
            return -1;
        }

        memset(st, 0, sizeof(*st));
        if (mp_conn_open(&st->conn, use_ssl, host, port) != 0) {
            PLOG_E("MUSIC", "连接失败: %s:%d", host, port);
            return -1;
        }

        char req[2560];
        char range_hdr[64] = "";
        if (range_offset > 0) {
            snprintf(range_hdr, sizeof(range_hdr), "Range: bytes=%llu-\r\n",
                     (unsigned long long)range_offset);
        }
        int rlen = snprintf(req, sizeof(req),
                            "GET %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "User-Agent: Mozilla/5.0 xiaozhi-zhiban/1.0\r\n"
                            "Accept: */*\r\n"
                            "%s"
                            "Connection: close\r\n\r\n",
                            path, host, range_hdr);
        if (rlen <= 0 || rlen >= (int)sizeof(req) || mp_conn_write(&st->conn, req, (size_t)rlen) < 0) {
            mp_conn_close(&st->conn);
            return -1;
        }

        char hdr_buf[4096];
        size_t hdr_len = 0;
        char *hdr_end = NULL;
        int timeout_loops = 0;

        while (!hdr_end && hdr_len < sizeof(hdr_buf) - 1) {
            if (should_abort_now()) {
                mp_conn_close(&st->conn);
                return -1;
            }
            int n = mp_conn_read(&st->conn, (uint8_t *)hdr_buf + hdr_len, sizeof(hdr_buf) - 1 - hdr_len);
            if (n == -2) {
                if (++timeout_loops > 30) {
                    mp_conn_close(&st->conn);
                    return -1;
                }
                continue;
            }
            if (n <= 0) {
                mp_conn_close(&st->conn);
                return -1;
            }
            timeout_loops = 0;
            hdr_len += (size_t)n;
            hdr_buf[hdr_len] = '\0';
            hdr_end = strstr(hdr_buf, "\r\n\r\n");
        }

        if (!hdr_end) {
            mp_conn_close(&st->conn);
            return -1;
        }

        *hdr_end = '\0';
        if (parse_http_headers(hdr_buf, st) != 0) {
            mp_conn_close(&st->conn);
            return -1;
        }

        if ((st->status_code == 301 || st->status_code == 302 ||
             st->status_code == 303 || st->status_code == 307 || st->status_code == 308) &&
            st->location[0]) {
            char next_url[MP_URL_MAX];
            mp_resolve_redirect(cur_url, st->location, next_url, sizeof(next_url));
            PLOG_I("MUSIC", "HTTP %d 重定向 -> %s", st->status_code, next_url);
            snprintf(cur_url, sizeof(cur_url), "%s", next_url);
            mp_conn_close(&st->conn);
            continue;
        }

        if (st->status_code != 200 && st->status_code != 206) {
            PLOG_W("MUSIC", "HTTP 状态异常: %d", st->status_code);
            mp_conn_close(&st->conn);
            return -1;
        }

        size_t body_off = (size_t)(hdr_end - hdr_buf) + 4;
        size_t extra = hdr_len - body_off;
        if (extra > sizeof(st->pre_buf))
            extra = sizeof(st->pre_buf);
        if (extra > 0) {
            memcpy(st->pre_buf, hdr_buf + body_off, extra);
            st->pre_len = extra;
            st->pre_pos = 0;
        }

        PLOG_I("MUSIC", "流已就绪: HTTP %d, cl=%lld, chunked=%d, range=%llu",
               st->status_code, (long long)st->content_length, st->chunked ? 1 : 0,
               (unsigned long long)range_offset);
        return 0;
    }

    PLOG_W("MUSIC", "重定向次数过多");
    return -1;
}

static int raw_read_byte(mp_http_stream_t *st, uint8_t *out)
{
    if (st->pre_pos < st->pre_len) {
        *out = st->pre_buf[st->pre_pos++];
        return 1;
    }
    int timeout_loops = 0;
    while (!should_abort_now()) {
        int n = mp_conn_read(&st->conn, st->pre_buf, sizeof(st->pre_buf));
        if (n == -2) {
            if (++timeout_loops > 30)
                return -1;
            continue;
        }
        if (n <= 0)
            return n; /* 0=EOF, <0=err */
        st->pre_len = (size_t)n;
        st->pre_pos = 1;
        *out = st->pre_buf[0];
        return 1;
    }
    return -1;
}

static int raw_read_chunk(mp_http_stream_t *st, uint8_t *buf, size_t max_len)
{
    if (st->pre_pos < st->pre_len) {
        size_t avail = st->pre_len - st->pre_pos;
        if (avail > max_len)
            avail = max_len;
        memcpy(buf, st->pre_buf + st->pre_pos, avail);
        st->pre_pos += avail;
        return (int)avail;
    }
    int timeout_loops = 0;
    while (!should_abort_now()) {
        int n = mp_conn_read(&st->conn, buf, max_len);
        if (n == -2) {
            if (++timeout_loops > 30)
                return -1;
            continue;
        }
        return n;
    }
    return -1;
}

static int mp_stream_read(mp_http_stream_t *st, uint8_t *buf, size_t max_len)
{
    if (!st->chunked) {
        if (st->content_length >= 0 && st->body_read >= st->content_length)
            return 0;
        size_t want = max_len;
        if (st->content_length >= 0) {
            int64_t rem = st->content_length - st->body_read;
            if ((int64_t)want > rem)
                want = (size_t)rem;
        }
        int n = raw_read_chunk(st, buf, want);
        if (n > 0)
            st->body_read += n;
        return n;
    }

    if (st->chunk_eof)
        return 0;

    if (st->chunk_rem == 0) {
        char line[64];
        size_t li = 0;
        while (li < sizeof(line) - 1) {
            uint8_t ch;
            int r = raw_read_byte(st, &ch);
            if (r <= 0)
                return r;
            if (ch == '\n')
                break;
            if (ch != '\r')
                line[li++] = (char)ch;
        }
        line[li] = '\0';
        if (li == 0) {
            /* 跳过上一个 chunk 结尾的空行后再读 size */
            while (li < sizeof(line) - 1) {
                uint8_t ch;
                int r = raw_read_byte(st, &ch);
                if (r <= 0)
                    return r;
                if (ch == '\n')
                    break;
                if (ch != '\r')
                    line[li++] = (char)ch;
            }
            line[li] = '\0';
        }
        long csz = strtol(line, NULL, 16);
        if (csz <= 0) {
            st->chunk_eof = true;
            return 0;
        }
        st->chunk_rem = csz;
    }

    size_t want = max_len;
    if ((int64_t)want > st->chunk_rem)
        want = (size_t)st->chunk_rem;
    int n = raw_read_chunk(st, buf, want);
    if (n > 0) {
        st->chunk_rem -= n;
        st->body_read += n;
        if (st->chunk_rem == 0) {
            uint8_t crlf;
            raw_read_byte(st, &crlf);
            if (crlf == '\r')
                raw_read_byte(st, &crlf);
        }
    }
    return n;
}

/* ---- 播放线程 ---- */

static void *mp_play_thread(void *arg)
{
    (void)arg;

    char url[MP_URL_MAX];
    uint64_t start_offset = 0;
    pthread_mutex_lock(&g_mp.lock);
    snprintf(url, sizeof(url), "%s", g_mp.url);
    start_offset = g_mp.resume_bytes;
    pthread_mutex_unlock(&g_mp.lock);

    mp_http_stream_t *st = (mp_http_stream_t *)calloc(1, sizeof(mp_http_stream_t));
    mp3dec_t *mp3d = (mp3dec_t *)malloc(sizeof(mp3dec_t));
    uint8_t *in_buf = (uint8_t *)malloc(MP_IN_BUF_SIZE);
    int16_t *pcm_buf = (int16_t *)malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    int32_t *s32_buf = (int32_t *)malloc(MP_S32_BUF_SAMPLES * sizeof(int32_t));

    if (!st || !mp3d || !in_buf || !pcm_buf || !s32_buf) {
        PLOG_E("MUSIC", "内存分配失败");
        goto cleanup;
    }
    mp3dec_init(mp3d);

    if (mp_stream_open(st, url, start_offset) != 0) {
        goto cleanup;
    }

    /* 若服务器不支持 Range(返回 200), 快进丢弃前 start_offset 字节 */
    if (start_offset > 0 && st->status_code == 200) {
        uint64_t to_skip = start_offset;
        while (to_skip > 0 && !should_abort_now()) {
            size_t want = to_skip > MP_IN_BUF_SIZE ? MP_IN_BUF_SIZE : (size_t)to_skip;
            int n = mp_stream_read(st, in_buf, want);
            if (n <= 0)
                break;
            to_skip -= (uint64_t)n;
        }
    }

    size_t in_bytes = 0;
    bool eof = false;
    bool id3_checked = (start_offset > 0);
    uint64_t id3_skip_rem = 0;

    void *track = NULL;
    int track_hz = 0;
    uint64_t cur_offset = start_offset;
    int stream_retries = 0;

    while (!should_abort_now()) {
        if (in_bytes < MP_IN_BUF_SIZE / 2 && !eof) {
            int n = mp_stream_read(st, in_buf + in_bytes, MP_IN_BUF_SIZE - in_bytes);
            if (n > 0) {
                in_bytes += (size_t)n;
                stream_retries = 0;
            } else {
                bool premature = (n < 0) || (st->content_length > 0 && st->body_read < st->content_length);
                if (premature && !should_abort_now() && stream_retries < 3) {
                    stream_retries++;
                    uint64_t next_off = cur_offset + in_bytes;
                    PLOG_W("MUSIC", "流中断(n=%d, read=%lld/%lld), Range重连(%d/3) offset=%llu",
                           n, (long long)st->body_read, (long long)st->content_length,
                           stream_retries, (unsigned long long)next_off);
                    mp_conn_close(&st->conn);
                    usleep(400000);
                    if (mp_stream_open(st, url, next_off) == 0) {
                        if (next_off > 0 && st->status_code == 200) {
                            uint64_t to_skip = next_off;
                            uint8_t skip_buf[2048];
                            while (to_skip > 0 && !should_abort_now()) {
                                size_t w = to_skip > sizeof(skip_buf) ? sizeof(skip_buf) : (size_t)to_skip;
                                int sn = mp_stream_read(st, skip_buf, w);
                                if (sn <= 0) break;
                                to_skip -= (uint64_t)sn;
                            }
                        }
                        continue;
                    }
                }
                if (n == 0) {
                    eof = true;
                } else {
                    break;
                }
            }
        }

        if (in_bytes == 0)
            break;

        /* 跳过文件头 ID3v2 标签 */
        if (!id3_checked && in_bytes >= 10) {
            id3_checked = true;
            if (in_buf[0] == 'I' && in_buf[1] == 'D' && in_buf[2] == '3') {
                uint32_t tag_sz = ((uint32_t)(in_buf[6] & 0x7F) << 21) |
                                  ((uint32_t)(in_buf[7] & 0x7F) << 14) |
                                  ((uint32_t)(in_buf[8] & 0x7F) << 7) |
                                  ((uint32_t)(in_buf[9] & 0x7F));
                id3_skip_rem = 10 + tag_sz;
                PLOG_I("MUSIC", "跳过 ID3v2 标签: %llu 字节", (unsigned long long)id3_skip_rem);
            }
        }
        if (id3_skip_rem > 0) {
            size_t drop = (in_bytes < id3_skip_rem) ? in_bytes : (size_t)id3_skip_rem;
            if (drop < in_bytes)
                memmove(in_buf, in_buf + drop, in_bytes - drop);
            in_bytes -= drop;
            id3_skip_rem -= drop;
            cur_offset += drop;
            continue;
        }

        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        int samples = mp3dec_decode_frame(mp3d, in_buf, (int)in_bytes, pcm_buf, &info);

        if (info.frame_bytes > 0) {
            size_t consumed = (size_t)info.frame_bytes;
            if (consumed < in_bytes)
                memmove(in_buf, in_buf + consumed, in_bytes - consumed);
            in_bytes -= consumed;
            cur_offset += consumed;
        } else if (eof) {
            break;
        } else {
            /* 数据不足等待下一批 */
            continue;
        }

        if (samples <= 0 || info.hz <= 0)
            continue;

        /* 等待 defer_start（等 AI 说完提示语回到 Idle） */
        while (!should_abort_now()) {
            bool deferred = false;
            pthread_mutex_lock(&g_mp.lock);
            deferred = g_mp.defer_start;
            if (deferred && g_mp.state != MUSIC_STATE_DEFERRED)
                g_mp.state = MUSIC_STATE_DEFERRED;
            g_mp.sample_rate = info.hz;
            pthread_mutex_unlock(&g_mp.lock);

            if (!deferred)
                break;
            usleep(40000);
        }
        if (should_abort_now())
            break;

        /* 打开或重建音频轨道（单声道输出，兼容 1X 单扬声器） */
        if (!track || track_hz != info.hz) {
            if (track) {
                audio_track_flush(track);
                audio_track_stop(track);
                audio_track_delete(track);
                track = NULL;
            }
            track = audio_track_create(NULL);
            if (!track) {
                PLOG_E("MUSIC", "audio_track_create 失败");
                break;
            }
            if (audio_track_init(track, 1, info.hz, AUDIO_TRACK_TYPE_MUSIC) != 0 ||
                audio_track_play(track) != 0) {
                PLOG_E("MUSIC", "audio_track_init/play 失败: hz=%d", info.hz);
                audio_track_delete(track);
                track = NULL;
                break;
            }
            track_hz = info.hz;
            PLOG_I("MUSIC", "音乐轨道已打开: hz=%d, src_ch=%d, offset=%llu",
                   info.hz, info.channels, (unsigned long long)cur_offset);
        }

        pthread_mutex_lock(&g_mp.lock);
        if (!g_mp.stop_req && !g_mp.pause_req) {
            g_mp.state = MUSIC_STATE_PLAYING;
        }
        g_mp.resume_bytes = cur_offset;
        g_mp.played_ms += (uint64_t)samples * 1000u / (uint64_t)info.hz;
        pthread_mutex_unlock(&g_mp.lock);

        /* 立体声混音为单声道并写入 audio_track */
        int offset = 0;
        while (offset < samples && !should_abort_now()) {
            int chunk = samples - offset;
            if (chunk > MP_S32_BUF_SAMPLES)
                chunk = MP_S32_BUF_SAMPLES;

            if (info.channels == 2) {
                for (int i = 0; i < chunk; i++) {
                    int idx = (offset + i) * 2;
                    s32_buf[i] = ((int32_t)pcm_buf[idx] + (int32_t)pcm_buf[idx + 1]) / 2;
                }
            } else {
                for (int i = 0; i < chunk; i++) {
                    s32_buf[i] = (int32_t)pcm_buf[offset + i];
                }
            }

            audio_track_write_params_t params;
            memset(&params, 0, sizeof(params));
            params.channel_data[0] = s32_buf;
            params.num_channels = 1;
            params.num_samples = chunk;
            params.shift_bits = 0;

            if (audio_track_write_data(track, &params) != 0) {
                PLOG_W("MUSIC", "audio_track_write_data 失败");
                break;
            }
            offset += chunk;
        }
    }

    if (track) {
        audio_track_flush(track);
        audio_track_stop(track);
        audio_track_delete(track);
        track = NULL;
    }

cleanup:
    if (st) {
        mp_conn_close(&st->conn);
        free(st);
    }
    free(mp3d);
    free(in_buf);
    free(pcm_buf);
    free(s32_buf);

    pthread_mutex_lock(&g_mp.lock);
    if (g_mp.pause_req) {
        g_mp.state = MUSIC_STATE_PAUSED;
        PLOG_I("MUSIC", "已暂停: offset=%llu, played=%llums",
               (unsigned long long)g_mp.resume_bytes, (unsigned long long)g_mp.played_ms);
    } else {
        g_mp.state = MUSIC_STATE_STOPPED;
        g_mp.resume_bytes = 0;
        g_mp.played_ms = 0;
        PLOG_I("MUSIC", "播放结束或已停止");
    }
    g_mp.thread_running = false;
    g_mp.stop_req = false;
    g_mp.pause_req = false;
    pthread_mutex_unlock(&g_mp.lock);

    return NULL;
}

/* ---- 对外接口 ---- */

static void stop_thread_sync(bool as_pause)
{
    pthread_t tid;
    bool need_join = false;

    pthread_mutex_lock(&g_mp.lock);
    if (g_mp.thread_running) {
        if (as_pause)
            g_mp.pause_req = true;
        else
            g_mp.stop_req = true;
        g_mp.defer_start = false;
        tid = g_mp.thread;
        need_join = true;
    } else if (!as_pause) {
        g_mp.state = MUSIC_STATE_STOPPED;
        g_mp.resume_bytes = 0;
        g_mp.played_ms = 0;
        g_mp.defer_start = false;
    }
    pthread_mutex_unlock(&g_mp.lock);

    if (need_join) {
        pthread_join(tid, NULL);
    }
}

static int start_play_thread(void)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024);

    pthread_mutex_lock(&g_mp.lock);
    g_mp.stop_req = false;
    g_mp.pause_req = false;
    g_mp.thread_running = true;
    int r = pthread_create(&g_mp.thread, &attr, mp_play_thread, NULL);
    if (r != 0) {
        g_mp.thread_running = false;
        g_mp.state = MUSIC_STATE_STOPPED;
    }
    pthread_mutex_unlock(&g_mp.lock);
    pthread_attr_destroy(&attr);
    return r == 0 ? 0 : -1;
}

int music_player_init(void)
{
    if (g_mp.initialized)
        return 0;
    memset(&g_mp, 0, sizeof(g_mp));
    pthread_mutex_init(&g_mp.lock, NULL);
    g_mp.initialized = true;
    PLOG_I("MUSIC", "音乐播放器初始化完成");
    return 0;
}

void music_player_destroy(void)
{
    if (!g_mp.initialized)
        return;
    stop_thread_sync(false);
    pthread_mutex_destroy(&g_mp.lock);
    g_mp.initialized = false;
}

int music_player_play_url(const char *url, const char *title, bool defer_start)
{
    if (!g_mp.initialized || !url || !url[0])
        return -1;

    stop_thread_sync(false);

    pthread_mutex_lock(&g_mp.lock);
    snprintf(g_mp.url, sizeof(g_mp.url), "%s", url);
    snprintf(g_mp.title, sizeof(g_mp.title), "%s", (title && title[0]) ? title : "未知歌曲");
    g_mp.resume_bytes = 0;
    g_mp.played_ms = 0;
    g_mp.defer_start = defer_start;
    g_mp.state = defer_start ? MUSIC_STATE_DEFERRED : MUSIC_STATE_BUFFERING;
    pthread_mutex_unlock(&g_mp.lock);

    PLOG_I("MUSIC", "准备播放: title=%s, defer=%d", g_mp.title, defer_start ? 1 : 0);
    return start_play_thread();
}

int music_player_pause(void)
{
    if (!g_mp.initialized)
        return -1;

    pthread_mutex_lock(&g_mp.lock);
    music_state_t st = g_mp.state;
    pthread_mutex_unlock(&g_mp.lock);

    if (st == MUSIC_STATE_PLAYING || st == MUSIC_STATE_BUFFERING || st == MUSIC_STATE_DEFERRED) {
        stop_thread_sync(true);
        return 0;
    }
    return -1;
}

int music_player_resume(bool defer_start)
{
    if (!g_mp.initialized)
        return -1;

    pthread_mutex_lock(&g_mp.lock);
    bool can_resume = (g_mp.state == MUSIC_STATE_PAUSED && g_mp.url[0] != '\0');
    if (can_resume) {
        g_mp.defer_start = defer_start;
        g_mp.state = defer_start ? MUSIC_STATE_DEFERRED : MUSIC_STATE_BUFFERING;
    }
    pthread_mutex_unlock(&g_mp.lock);

    if (!can_resume)
        return -1;

    PLOG_I("MUSIC", "恢复播放: title=%s, offset=%llu",
           g_mp.title, (unsigned long long)g_mp.resume_bytes);
    return start_play_thread();
}

int music_player_stop(void)
{
    if (!g_mp.initialized)
        return -1;
    stop_thread_sync(false);
    return 0;
}

void music_player_on_idle(void)
{
    if (!g_mp.initialized)
        return;
    pthread_mutex_lock(&g_mp.lock);
    if (g_mp.defer_start) {
        g_mp.defer_start = false;
        PLOG_I("MUSIC", "会话已回到 Idle，解除 defer_start 开始放歌");
    }
    pthread_mutex_unlock(&g_mp.lock);
}

bool music_player_is_playing(void)
{
    if (!g_mp.initialized)
        return false;
    pthread_mutex_lock(&g_mp.lock);
    bool playing = (g_mp.state == MUSIC_STATE_PLAYING);
    pthread_mutex_unlock(&g_mp.lock);
    return playing;
}

bool music_player_has_deferred(void)
{
    if (!g_mp.initialized)
        return false;
    pthread_mutex_lock(&g_mp.lock);
    bool def = (g_mp.defer_start && g_mp.thread_running);
    pthread_mutex_unlock(&g_mp.lock);
    return def;
}

music_state_t music_player_get_state(void)
{
    if (!g_mp.initialized)
        return MUSIC_STATE_STOPPED;
    pthread_mutex_lock(&g_mp.lock);
    music_state_t st = g_mp.state;
    pthread_mutex_unlock(&g_mp.lock);
    return st;
}

void music_player_get_status(char *buf, size_t size)
{
    if (!buf || size == 0)
        return;
    const char *names[] = { "stopped", "buffering", "deferred", "playing", "paused" };
    pthread_mutex_lock(&g_mp.lock);
    int st = (int)g_mp.state;
    if (st < 0 || st > 4)
        st = 0;
    snprintf(buf, size, "state=%s title=%s played=%llus",
             names[st], g_mp.title[0] ? g_mp.title : "none",
             (unsigned long long)(g_mp.played_ms / 1000));
    pthread_mutex_unlock(&g_mp.lock);
}
