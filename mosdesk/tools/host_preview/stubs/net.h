#pragma once
/* Host stub of the SDK's net.h: the status call the panel's network button
 * makes. The host has no network, so it reports no adapter. */
#include "syscall.h"
#define SYS_NET_STATUS 1u
typedef struct {
    uint8_t  has_driver;
    uint8_t  configured;
    uint8_t  mac[6];
    uint32_t local_ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
} syscall_net_status_t;
typedef struct {
    uint32_t op;
    void* buf;
    uint32_t len;
    syscall_net_status_t* out_status;
} syscall_net_request_t;
static inline long mos_net_call(syscall_net_request_t* request) {
    (void)request;
    return -1;
}
static inline long mos_net_status(syscall_net_status_t* out) {
    (void)out;
    return -1;
}
