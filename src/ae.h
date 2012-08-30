/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __AE_H__
#define __AE_H__

// 运行状态返回值
#define AE_OK 0
#define AE_ERR -1

// 事件动作类型
#define AE_NONE 0
#define AE_READABLE 1
#define AE_WRITABLE 2

// 事件类型
#define AE_FILE_EVENTS 1
#define AE_TIME_EVENTS 2
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT 4

// 当时间事件的返回值不是 AE_NOMORE 时
// 将时间事件放到稍候再执行
#define AE_NOMORE -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

// 向前声明
struct aeEventLoop;

/* Types and data structures */
// 事件处理器的函数原型
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* File event structure */
// 文件事件
typedef struct aeFileEvent {
    // 动作类型
    int mask; /* one of AE_(READABLE|WRITABLE) */
    // 事件处理函数
    aeFileProc *rfileProc;
    aeFileProc *wfileProc;
    // API 数据
    void *clientData;
} aeFileEvent;

/* Time event structure */
// 时间事件
typedef struct aeTimeEvent {
    // 时间事件 id
    long long id; /* time event identifier. */
    // 执行时间
    long when_sec; /* seconds */
    long when_ms; /* milliseconds */
    // 事件处理函数
    aeTimeProc *timeProc;
    // 事件被删除时执行的清理函数
    aeEventFinalizerProc *finalizerProc;
    // API 数据
    void *clientData;
    // 指向下一个时间事件，形成链表
    struct aeTimeEvent *next;
} aeTimeEvent;

/* A fired event */
// 已就绪文件事件（被触发，等待处理，不阻塞）
typedef struct aeFiredEvent {
    int fd;
    int mask;
} aeFiredEvent;

/* State of an event based program */
// 事件循环（状态）
typedef struct aeEventLoop {
    // 当前已注册的最大文件描述符
    int maxfd;   /* highest file descriptor currently registered */
    // 允许注册的最大文件描述符数量
    int setsize; /* max number of file descriptors tracked */
    // 时间事件的 id 计数器（用于生成新 id ）
    long long timeEventNextId;
    // 文件事件数组
    aeFileEvent *events; /* Registered events */
    // 已就绪文件事件数组
    aeFiredEvent *fired; /* Fired events */
    // 时间事件（链表）
    aeTimeEvent *timeEventHead;
    // 停止事件处理？
    int stop;
    // poll API 所需的数据
    void *apidata; /* This is used for polling API specific data */
    // 在每次开始处理事件前要执行的函数
    aeBeforeSleepProc *beforesleep;
} aeEventLoop;

/* Prototypes */
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);

#endif
