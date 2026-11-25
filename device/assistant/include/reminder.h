#ifndef REMINDER_H
#define REMINDER_H

#include <stddef.h>
#include <stdint.h>

int reminder_init(void);
void reminder_destroy(void);

int reminder_add(const char *title, int delay_seconds, const char *trigger_time,
                 const char *repeat, char *out_msg, size_t out_sz);
void reminder_list(char *out_msg, size_t out_sz);
int reminder_delete(int id);

/* 轮询到期提醒，返回 1 表示有提醒到期并写入 out_title */
int reminder_poll(char *out_title, size_t out_sz);

#endif
