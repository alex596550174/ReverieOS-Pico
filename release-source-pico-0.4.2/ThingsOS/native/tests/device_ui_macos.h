// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* The controller's Host scan tests use only manual-reset events and a
 * suspended worker. Keep that contract while using system threads on macOS. */
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef void* LPVOID;
#define WINAPI
#define TRUE 1
#define FALSE 0
#define CREATE_SUSPENDED 4
#define INFINITE UINT32_MAX
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED UINT32_MAX

typedef struct host_waitable {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    DWORD (*worker)(LPVOID);
    LPVOID argument;
    bool signaled;
    bool started;
} *HANDLE;

static HANDLE host_waitable_create(bool initial) {
    HANDLE item=calloc(1,sizeof(*item));
    if(!item)return NULL;
    if(pthread_mutex_init(&item->mutex,NULL)) {
        free(item);
        return NULL;
    }
    if(pthread_cond_init(&item->changed,NULL)) {
        pthread_mutex_destroy(&item->mutex);
        free(item);
        return NULL;
    }
    item->signaled=initial;
    return item;
}

static void host_waitable_destroy(HANDLE item) {
    pthread_cond_destroy(&item->changed);
    pthread_mutex_destroy(&item->mutex);
    free(item);
}

static void SetEvent(HANDLE item) {
    pthread_mutex_lock(&item->mutex);
    item->signaled=true;
    pthread_cond_broadcast(&item->changed);
    pthread_mutex_unlock(&item->mutex);
}

#define CreateEvent(attributes, manual, initial, name) host_waitable_create(initial)

static DWORD WaitForSingleObject(HANDLE item,DWORD milliseconds) {
    struct timespec deadline;
    if(milliseconds!=INFINITE) {
        if(clock_gettime(CLOCK_REALTIME,&deadline))return WAIT_FAILED;
        deadline.tv_sec+=(time_t)(milliseconds/1000u);
        deadline.tv_nsec+=(long)(milliseconds%1000u)*1000000L;
        if(deadline.tv_nsec>=1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec-=1000000000L;
        }
    }

    pthread_mutex_lock(&item->mutex);
    int result=0;
    while(!item->signaled && !result) {
        if(milliseconds==INFINITE)result=pthread_cond_wait(&item->changed,&item->mutex);
        else result=pthread_cond_timedwait(&item->changed,&item->mutex,&deadline);
    }
    bool signaled=item->signaled;
    pthread_mutex_unlock(&item->mutex);
    return signaled?WAIT_OBJECT_0:result==ETIMEDOUT?WAIT_TIMEOUT:WAIT_FAILED;
}

static void* host_thread_entry(void* argument) {
    HANDLE item=argument;
    pthread_mutex_lock(&item->mutex);
    while(!item->started)pthread_cond_wait(&item->changed,&item->mutex);
    pthread_mutex_unlock(&item->mutex);
    item->worker(item->argument);
    SetEvent(item);
    return NULL;
}

static HANDLE host_thread_create(DWORD (*worker)(LPVOID),LPVOID argument) {
    HANDLE item=host_waitable_create(false);
    if(!item)return NULL;
    item->worker=worker;
    item->argument=argument;
    if(pthread_create(&item->thread,NULL,host_thread_entry,item)) {
        host_waitable_destroy(item);
        return NULL;
    }
    return item;
}

#define CreateThread(attributes, stack, worker, argument, flags, id) host_thread_create(worker,argument)

static void ResumeThread(HANDLE item) {
    pthread_mutex_lock(&item->mutex);
    item->started=true;
    pthread_cond_broadcast(&item->changed);
    pthread_mutex_unlock(&item->mutex);
}

static void CloseHandle(HANDLE item) {
    if(item->worker)pthread_join(item->thread,NULL);
    host_waitable_destroy(item);
}

static void Sleep(DWORD milliseconds) {
    struct timespec left={(time_t)(milliseconds/1000u),(long)(milliseconds%1000u)*1000000L};
    while(nanosleep(&left,&left) && errno==EINTR) {}
}

static LONG InterlockedExchange(volatile LONG* value,LONG replacement) {
    return __atomic_exchange_n(value,replacement,__ATOMIC_SEQ_CST);
}

static LONG InterlockedCompareExchange(volatile LONG* value,LONG replacement,LONG expected) {
    __atomic_compare_exchange_n(value,&expected,replacement,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    return expected;
}
