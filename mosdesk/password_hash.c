#include <stddef.h>
#include <string.h>

#include "password_hash.h"
/* SHA-256 from minimaSSL. MOSdesk's own SDK submodule when it is checked
 * out; otherwise the SDK doing the build (Minimal-OS builds MOSdesk with
 * its own, MOSDESK_SDK_DIR), found through that SDK's include directory. */
#if __has_include("../Minimal-OS-SDK/libraries/minimaSSL/src/crypto.c")
#include "../Minimal-OS-SDK/libraries/minimaSSL/src/crypto.c"
#else
#include "../libraries/minimaSSL/src/crypto.c"
#endif

#define PASSWORD_HASH_ROUNDS 4096u
#define PASSWORD_MAX_LENGTH 63u

static void password_hmac(const uint8_t key[64], const uint8_t* data,
                          size_t length, uint8_t digest[32]) {
    uint8_t inner[64 + 32];
    uint8_t outer[64 + 32];
    for (size_t i = 0; i < 64u; i++) {
        inner[i] = (uint8_t)(key[i] ^ 0x36u);
        outer[i] = (uint8_t)(key[i] ^ 0x5Cu);
    }
    memcpy(inner + 64u, data, length);
    dlr_sha256(inner, 64u + length, digest);
    memcpy(outer + 64u, digest, 32u);
    dlr_sha256(outer, sizeof(outer), digest);
}

static void password_clear(void* memory, size_t size) {
    volatile uint8_t* bytes = (volatile uint8_t*)memory;
    while (size--) *bytes++ = 0;
}

bool mosdesk_password_hash(const uint8_t salt[MOSDESK_PASSWORD_SALT_SIZE],
                           const char* password,
                           uint8_t digest[MOSDESK_PASSWORD_HASH_SIZE]) {
    if (!salt || !password || !digest) return false;
    size_t length = strlen(password);
    if (length == 0u || length > PASSWORD_MAX_LENGTH) return false;

    uint8_t key[64] = {0};
    memcpy(key, password, length);
    uint8_t block[MOSDESK_PASSWORD_SALT_SIZE + 4u];
    memcpy(block, salt, MOSDESK_PASSWORD_SALT_SIZE);
    block[MOSDESK_PASSWORD_SALT_SIZE + 0u] = 0;
    block[MOSDESK_PASSWORD_SALT_SIZE + 1u] = 0;
    block[MOSDESK_PASSWORD_SALT_SIZE + 2u] = 0;
    block[MOSDESK_PASSWORD_SALT_SIZE + 3u] = 1;

    uint8_t value[32];
    password_hmac(key, block, sizeof(block), value);
    memcpy(digest, value, sizeof(value));
    for (uint32_t round = 1; round < PASSWORD_HASH_ROUNDS; round++) {
        password_hmac(key, value, sizeof(value), value);
        for (size_t i = 0; i < sizeof(value); i++)
            digest[i] ^= value[i];
    }
    password_clear(key, sizeof(key));
    password_clear(block, sizeof(block));
    password_clear(value, sizeof(value));
    return true;
}
