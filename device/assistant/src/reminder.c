/**
 * @file reminder.c
 * @brief 本地定时提醒管理（持久化到 /var/upgrade/reminders.json）
 */

#include "reminder.h"
#include "cJSON.h"
#include "plog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#define REMINDER_FILE       "/var/upgrade/reminders.json"
#define REMINDER_MAX_ITEMS  32
#define REMINDER_TZ_OFFSET  (8 * 3600) /* 默认按北京时间 UTC+8 解析/显示 */

typedef struct {
    int id;
    int64_t trigger_epoch;
    char title[160];
    int daily;
    int enabled;
} reminder_item_t;

static struct {
    pthread_mutex_t lock;
    reminder_item_t items[REMINDER_MAX_ITEMS];
    int count;
    int next_id;
    int initialized;
} g_rem;

static int get_tz_offset_sec(void)
{
    /* 设备系统时钟本身已是本地北京时间（只是未设 TZ 变量），无需再额外加减 8 小时 */
    return 0;
}

static void epoch_to_local_str(int64_t epoch, char *buf, size_t sz)
{
    time_t adj = (time_t)(epoch + get_tz_offset_sec());
    struct tm tm_v;
    gmtime_r(&adj, &tm_v);
    snprintf(buf, sz, "%04d-%02d-%02d %02d:%02d:%02d",
             tm_v.tm_year + 1900, tm_v.tm_mon + 1, tm_v.tm_mday,
             tm_v.tm_hour, tm_v.tm_min, tm_v.tm_sec);
}

static int parse_trigger_time_to_epoch(const char *str, int64_t *out_epoch)
{
    if (!str || !str[0])
        return -1;

    int tz_off = get_tz_offset_sec();
    time_t now_utc = time(NULL);
    time_t now_local = now_utc + tz_off;
    struct tm now_tm;
    gmtime_r(&now_local, &now_tm);

    int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(str, "%d-%d-%d %d:%d:%d", &y, &m, &d, &hh, &mm, &ss) >= 5 ||
        sscanf(str, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &hh, &mm, &ss) >= 5) {
        struct tm t = {0};
        t.tm_year = y - 1900;
        t.tm_mon = m - 1;
        t.tm_mday = d;
        t.tm_hour = hh;
        t.tm_min = mm;
        t.tm_sec = ss;
        /* 用 timegm 等价逻辑：临时清 TZ 或手动计算 */
        char *old_tz = getenv("TZ");
        char saved_tz[64] = {0};
        if (old_tz) strncpy(saved_tz, old_tz, sizeof(saved_tz) - 1);
        setenv("TZ", "UTC", 1);
        tzset();
        time_t local_ep = mktime(&t);
        if (old_tz) setenv("TZ", saved_tz, 1);
        else unsetenv("TZ");
        tzset();
        if (local_ep < 0)
            return -1;
        *out_epoch = (int64_t)local_ep - tz_off;
        return 0;
    }

    if (sscanf(str, "%d:%d:%d", &hh, &mm, &ss) >= 2) {
        if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59)
            return -1;
        struct tm t = now_tm;
        t.tm_hour = hh;
        t.tm_min = mm;
        t.tm_sec = ss;
        char *old_tz = getenv("TZ");
        char saved_tz[64] = {0};
        if (old_tz) strncpy(saved_tz, old_tz, sizeof(saved_tz) - 1);
        setenv("TZ", "UTC", 1);
        tzset();
        time_t local_ep = mktime(&t);
        if (old_tz) setenv("TZ", saved_tz, 1);
        else unsetenv("TZ");
        tzset();
        if (local_ep < 0)
            return -1;
        int64_t target = (int64_t)local_ep - tz_off;
        if (target <= (int64_t)now_utc)
            target += 86400; /* 今天已过则顺延到明天 */
        *out_epoch = target;
        return 0;
    }

    return -1;
}

static void save_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddNumberToObject(root, "next_id", g_rem.next_id);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < g_rem.count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", g_rem.items[i].id);
        cJSON_AddNumberToObject(obj, "trigger_epoch", (double)g_rem.items[i].trigger_epoch);
        cJSON_AddStringToObject(obj, "title", g_rem.items[i].title);
        cJSON_AddBoolToObject(obj, "daily", g_rem.items[i].daily ? 1 : 0);
        cJSON_AddBoolToObject(obj, "enabled", g_rem.items[i].enabled ? 1 : 0);
        cJSON_AddItemToArray(arr, obj);
    }
    cJSON_AddItemToObject(root, "items", arr);

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return;

    FILE *f = fopen(REMINDER_FILE ".tmp", "w");
    if (f) {
        fwrite(txt, 1, strlen(txt), f);
        fclose(f);
        unlink(REMINDER_FILE);
        rename(REMINDER_FILE ".tmp", REMINDER_FILE);
    }
    free(txt);
}

static void load_locked(void)
{
    g_rem.count = 0;
    g_rem.next_id = 1;

    FILE *f = fopen(REMINDER_FILE, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 32768) {
        fclose(f);
        return;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *nid = cJSON_GetObjectItem(root, "next_id");
    if (nid && cJSON_IsNumber(nid) && nid->valueint > 0)
        g_rem.next_id = nid->valueint;

    cJSON *arr = cJSON_GetObjectItem(root, "items");
    if (arr && cJSON_IsArray(arr)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, arr) {
            if (g_rem.count >= REMINDER_MAX_ITEMS) break;
            cJSON *jid = cJSON_GetObjectItem(it, "id");
            cJSON *jep = cJSON_GetObjectItem(it, "trigger_epoch");
            cJSON *jti = cJSON_GetObjectItem(it, "title");
            cJSON *jda = cJSON_GetObjectItem(it, "daily");
            cJSON *jen = cJSON_GetObjectItem(it, "enabled");
            if (!jid || !jep || !jti || !cJSON_IsString(jti)) continue;

            reminder_item_t *dst = &g_rem.items[g_rem.count++];
            memset(dst, 0, sizeof(*dst));
            dst->id = jid->valueint;
            dst->trigger_epoch = (int64_t)jep->valuedouble;
            snprintf(dst->title, sizeof(dst->title), "%s", jti->valuestring);
            dst->daily = (jda && cJSON_IsTrue(jda)) ? 1 : 0;
            dst->enabled = (!jen || cJSON_IsTrue(jen)) ? 1 : 0;
            if (dst->id >= g_rem.next_id)
                g_rem.next_id = dst->id + 1;
        }
    }
    cJSON_Delete(root);
}

int reminder_init(void)
{
    if (g_rem.initialized) return 0;
    memset(&g_rem, 0, sizeof(g_rem));
    pthread_mutex_init(&g_rem.lock, NULL);
    pthread_mutex_lock(&g_rem.lock);
    load_locked();
    g_rem.initialized = 1;
    pthread_mutex_unlock(&g_rem.lock);
    PLOG_I("REMINDER", "初始化完成, 已加载 %d 条提醒", g_rem.count);
    return 0;
}

void reminder_destroy(void)
{
    if (!g_rem.initialized) return;
    pthread_mutex_destroy(&g_rem.lock);
    g_rem.initialized = 0;
}

int reminder_add(const char *title, int delay_seconds, const char *trigger_time,
                 const char *repeat, char *out_msg, size_t out_sz)
{
    if (!g_rem.initialized || !title || !title[0]) {
        snprintf(out_msg, out_sz, "invalid reminder title");
        return -1;
    }

    int64_t target = 0;
    if (delay_seconds > 0) {
        target = (int64_t)time(NULL) + delay_seconds;
    } else if (trigger_time && trigger_time[0]) {
        if (parse_trigger_time_to_epoch(trigger_time, &target) != 0) {
            snprintf(out_msg, out_sz, "invalid trigger_time format");
            return -1;
        }
    } else {
        snprintf(out_msg, out_sz, "need delay_seconds or trigger_time");
        return -1;
    }

    int daily = (repeat && strcasecmp(repeat, "daily") == 0) ? 1 : 0;

    pthread_mutex_lock(&g_rem.lock);
    if (g_rem.count >= REMINDER_MAX_ITEMS) {
        pthread_mutex_unlock(&g_rem.lock);
        snprintf(out_msg, out_sz, "reminder list full (max %d)", REMINDER_MAX_ITEMS);
        return -1;
    }

    reminder_item_t *it = &g_rem.items[g_rem.count++];
    memset(it, 0, sizeof(*it));
    it->id = g_rem.next_id++;
    it->trigger_epoch = target;
    snprintf(it->title, sizeof(it->title), "%s", title);
    it->daily = daily;
    it->enabled = 1;
    save_locked();

    char tbuf[32];
    epoch_to_local_str(target, tbuf, sizeof(tbuf));
    snprintf(out_msg, out_sz, "reminder #%d set for %s%s: %s",
             it->id, tbuf, daily ? " (daily)" : "", it->title);
    PLOG_I("REMINDER", "%s", out_msg);
    pthread_mutex_unlock(&g_rem.lock);
    return 0;
}

void reminder_list(char *out_msg, size_t out_sz)
{
    if (!g_rem.initialized) {
        snprintf(out_msg, out_sz, "no reminders");
        return;
    }
    pthread_mutex_lock(&g_rem.lock);
    if (g_rem.count == 0) {
        pthread_mutex_unlock(&g_rem.lock);
        snprintf(out_msg, out_sz, "no active reminders");
        return;
    }
    size_t pos = 0;
    for (int i = 0; i < g_rem.count && pos < out_sz - 1; i++) {
        char tbuf[32];
        epoch_to_local_str(g_rem.items[i].trigger_epoch, tbuf, sizeof(tbuf));
        int n = snprintf(out_msg + pos, out_sz - pos, "#%d [%s%s] %s; ",
                         g_rem.items[i].id, tbuf,
                         g_rem.items[i].daily ? " daily" : "",
                         g_rem.items[i].title);
        if (n > 0) pos += (size_t)n;
    }
    pthread_mutex_unlock(&g_rem.lock);
}

int reminder_delete(int id)
{
    if (!g_rem.initialized) return -1;
    pthread_mutex_lock(&g_rem.lock);
    if (id <= 0) {
        g_rem.count = 0;
        save_locked();
        pthread_mutex_unlock(&g_rem.lock);
        PLOG_I("REMINDER", "已清空全部提醒");
        return 0;
    }
    int found = 0;
    for (int i = 0; i < g_rem.count; i++) {
        if (g_rem.items[i].id == id) {
            for (int j = i; j < g_rem.count - 1; j++)
                g_rem.items[j] = g_rem.items[j + 1];
            g_rem.count--;
            found = 1;
            break;
        }
    }
    if (found) save_locked();
    pthread_mutex_unlock(&g_rem.lock);
    return found ? 0 : -1;
}

int reminder_poll(char *out_title, size_t out_sz)
{
    if (!g_rem.initialized || !out_title || out_sz == 0)
        return 0;

    int64_t now = (int64_t)time(NULL);
    int triggered = 0;
    int dirty = 0;

    pthread_mutex_lock(&g_rem.lock);
    for (int i = 0; i < g_rem.count; ) {
        reminder_item_t *it = &g_rem.items[i];
        if (!it->enabled) {
            i++;
            continue;
        }
        if (now >= it->trigger_epoch) {
            int64_t late = now - it->trigger_epoch;
            if (late > 300) {
                /* 过期超过5分钟（如断电期间错过）：daily 顺延，单次丢弃 */
                if (it->daily) {
                    while (it->trigger_epoch <= now)
                        it->trigger_epoch += 86400;
                    i++;
                } else {
                    for (int j = i; j < g_rem.count - 1; j++)
                        g_rem.items[j] = g_rem.items[j + 1];
                    g_rem.count--;
                }
                dirty = 1;
                continue;
            }
            if (!triggered) {
                snprintf(out_title, out_sz, "%s", it->title);
                triggered = 1;
                PLOG_I("REMINDER", "触发提醒 #%d: %s", it->id, it->title);
                if (it->daily) {
                    it->trigger_epoch += 86400;
                    i++;
                } else {
                    for (int j = i; j < g_rem.count - 1; j++)
                        g_rem.items[j] = g_rem.items[j + 1];
                    g_rem.count--;
                }
                dirty = 1;
                break;
            }
        }
        i++;
    }
    if (dirty) save_locked();
    pthread_mutex_unlock(&g_rem.lock);
    return triggered;
}
