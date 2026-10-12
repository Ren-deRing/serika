#pragma once

#include <serika/wait.h>

#include <stdint.h>

#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

struct pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct poll_table;

/* from an inode's poll(): the queue whose wake_up() may change the answer. at most two a call. NULL pt: just asking */
void poll_wait(struct poll_table *pt, struct wait_queue *wq);
