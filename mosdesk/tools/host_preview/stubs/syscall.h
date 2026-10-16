#pragma once
/* Host stub of the Minimal-OS SDK syscall header: only what MOSdesk uses. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define SYS_SUCCESS 0ull
#define SYS_ERR_AGAIN ((uint64_t)-2)
#define SYS_ERR_BUSY ((uint64_t)-3)
#define SYS_OPEN 10u
#define SYS_READ 11u
#define SYS_CLOSE 12u
#define SYS_SIZE 13u
#define SYS_SEEK 14u
#define SYS_FWRITE 15u
#define SYS_GETTIME 16u
#define SYS_UPTIME 4u
#define SYS_HEAP 17u
#define SYS_O_RDONLY 0u
uint64_t mos_syscall(uint64_t n, uint64_t a, uint64_t b, uint64_t c);
uint64_t mos_syscall1(uint64_t n, uint64_t a);
uint64_t mos_syscall3(uint64_t n, uint64_t a, uint64_t b, uint64_t c);
/* --- additions: newer syscalls used by current mosWM --- */
#define SYS_ERR_NOTFOUND ((uint64_t)-4)
#define SYS_RANDOM 90u
#define SYS_USB 91u
#define SYS_USB_KEY_TO_ASCII 1u
#define SYS_LISTDIR 92u
#define SYS_EXEC 33u
#define SYSCALL_DIRENT_TYPE_FILE 1u
#define SYSCALL_DIRENT_TYPE_DIR 2u
#define SYSCALL_DIRENT_TYPE_EXECUTABLE 3u
#define SYSCALL_DIRENT_NAME_SIZE 64u
typedef struct { char name[64]; uint32_t type; uint32_t hidden; } syscall_dirent_t;
/* --- additions: file operations and right-click used by the shell --- */
#define SYS_EXISTS 9u
#define SYS_RMDIR 21u
#define SYS_GET_METADATA 22u
/* --- additions: the user folders (0:/users/<username>/...) --- */
#define SYS_IS_DIR 93u
#define SYS_MKDIR 94u
#define SYSCALL_MOUSE_BTN_RIGHT (1u << 1)
typedef struct {
    char filetype[64];
    char fileformat[16];
    uint32_t data_length;
    uint8_t runnable;
    uint8_t hidden;
    uint64_t entrypoint;
    char created_date[32];
    char last_changed[32];
} syscall_file_metadata_t;
