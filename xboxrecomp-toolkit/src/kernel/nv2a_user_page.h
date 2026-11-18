#ifndef NV2A_USER_PAGE_H
#define NV2A_USER_PAGE_H

#include "platform/xbox_winnt.h"
#include <stddef.h>

#define NV2A_USER_PAGE_OFFSET 0x800000u
#define NV2A_USER_PAGE_BYTES  0x10000u

typedef void *(*Nv2aUserMapReadOnly)(HANDLE mapping, size_t size, void *address);
typedef struct {
    HANDLE mapping;
    void *guest;
    void *host;
    void *handler;
} Nv2aUserPage;

int nv2a_user_page_create(Nv2aUserPage *page, void *address, Nv2aUserMapReadOnly map);
void nv2a_user_page_destroy(Nv2aUserPage *page);

#endif
