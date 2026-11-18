#include "nv2a_user_page.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
static Nv2aUserPage *s_page;

static LONG CALLBACK untracked_user_write(PEXCEPTION_POINTERS ep)
{
    uintptr_t at, base;
    if (!s_page || ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        ep->ExceptionRecord->NumberParameters < 2 ||
        ep->ExceptionRecord->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    at = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    base = (uintptr_t)s_page->guest;
    if (at < base || at - base >= NV2A_USER_PAGE_BYTES)
        return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "[NV2A-SEGMENT] untracked guest USER write at offset 0x%04llX;"
                    " refusing to lose a publication or reset\n",
            (unsigned long long)(at - base));
    fflush(stderr);
    abort();
    return EXCEPTION_CONTINUE_SEARCH;
}

int nv2a_user_page_create(Nv2aUserPage *page, void *address, Nv2aUserMapReadOnly map)
{
    MEMORY_BASIC_INFORMATION info;
    if (!page || !map || s_page)
        return 0;
    memset(page, 0, sizeof *page);
    page->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                       0, NV2A_USER_PAGE_BYTES, NULL);
    if (!page->mapping)
        goto failed;
    page->host = MapViewOfFile(page->mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                               0, 0, NV2A_USER_PAGE_BYTES);
    if (!page->host)
        goto failed;
    page->guest = map(page->mapping, NV2A_USER_PAGE_BYTES, address);
    if (!page->guest || (address && page->guest != address))
        goto failed;
    if (VirtualQuery(page->guest, &info, sizeof info) != sizeof info ||
        info.State != MEM_COMMIT || (info.Protect & 0xFFu) != PAGE_READONLY)
        goto failed;
    s_page = page;
    page->handler = AddVectoredExceptionHandler(1, untracked_user_write);
    if (!page->handler)
        goto failed;
    return 1;
failed:
    fprintf(stderr, "[NV2A-SEGMENT] cannot establish read-only USER guard:"
                    " error=%lu\n", (unsigned long)GetLastError());
    fflush(stderr);
    nv2a_user_page_destroy(page);
    return 0;
}

void nv2a_user_page_destroy(Nv2aUserPage *page)
{
    if (!page) return;
    if (page->handler) RemoveVectoredExceptionHandler(page->handler);
    if (s_page == page) s_page = NULL;
    if (page->guest) UnmapViewOfFile(page->guest);
    if (page->host) UnmapViewOfFile(page->host);
    if (page->mapping) CloseHandle(page->mapping);
    memset(page, 0, sizeof *page);
}
#else
int nv2a_user_page_create(Nv2aUserPage *page, void *address, Nv2aUserMapReadOnly map)
{
    (void)page; (void)address; (void)map;
    fprintf(stderr, "[NV2A-SEGMENT] guarded USER mapping is not supported on this host\n");
    return 0;
}
void nv2a_user_page_destroy(Nv2aUserPage *page) { (void)page; }
#endif
