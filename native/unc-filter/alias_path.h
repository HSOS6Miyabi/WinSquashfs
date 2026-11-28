/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ARCHIVEFS_ALIAS_PATH_H
#define ARCHIVEFS_ALIAS_PATH_H

#include <stddef.h>
#include <stdint.h>

/*
 * Pure UTF-16 name handling shared by the kernel filter and its tests.
 * The input is a MUP-relative file object name, not a Win32 path.
 * No allocation, I/O, locale-dependent comparison or string terminator is needed.
 */
typedef enum ArchiveFsAliasResult {
    ArchiveFsAliasUnrelated,
    ArchiveFsAliasInvalid,
    ArchiveFsAliasMatched
} ArchiveFsAliasResult;

typedef struct ArchiveFsAlias {
    size_t host_begin;
    size_t host_length;
    size_t share_begin;
    size_t target_length;
} ArchiveFsAlias;

static inline uint16_t ArchiveFsLower(uint16_t ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (uint16_t)(ch + ('a' - 'A')) : ch;
}

static inline int ArchiveFsAsciiEqual(const uint16_t *s, size_t n, const char *literal)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (!literal[i] || ArchiveFsLower(s[i]) != (uint16_t)(unsigned char)literal[i])
            return 0;
    }
    return literal[n] == 0;
}

static inline int ArchiveFsHostValid(const uint16_t *s, size_t n)
{
    size_t i, label = 0;
    if (!n || n > 253 || s[0] == '.' || s[n - 1] == '.')
        return 0;
    /* The reserved namespace must not be used as a recursive source host. */
    if (n >= 4 && ArchiveFsAsciiEqual(s, 4, "sfs."))
        return 0;
    for (i = 0; i < n; ++i) {
        uint16_t ch = ArchiveFsLower(s[i]);
        if (ch == '.') {
            if (!label || s[i - 1] == '-') return 0;
            label = 0;
        } else {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                  ch == '-' || ch == '_'))
                return 0;
            if (!label && ch == '-') return 0;
            if (++label > 63) return 0;
        }
    }
    return label && s[n - 1] != '-';
}

static inline int ArchiveFsPathValid(const uint16_t *s, size_t n)
{
    size_t i, begin = 0;
    for (i = 0; i <= n; ++i) {
        if (i == n || s[i] == '\\') {
            size_t end = i;
            if (end == begin) return i == n && i != 0;
            /* Win32 may normalize trailing dots/spaces. Reject such
             * components, including disguised traversal, before reparsing. */
            if (s[end - 1] == '.' || s[end - 1] == ' ' || end - begin > 255)
                return 0;
            begin = i + 1;
        } else {
            uint16_t ch = s[i];
            if (ch < 32 || ch == ':' || ch == '/' || ch == '*' ||
                ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
                return 0;
            if (ch >= 0xd800 && ch <= 0xdbff) {
                if (i + 1 >= n || s[i + 1] < 0xdc00 || s[i + 1] > 0xdfff)
                    return 0;
                ++i;
            } else if (ch >= 0xdc00 && ch <= 0xdfff) {
                return 0;
            }
        }
    }
    return 1;
}

#define ARCHIVEFS_ALIAS_TARGET "\\Device\\Mup\\sfs$\\UNC\\"
#define ARCHIVEFS_ALIAS_TARGET_LENGTH (sizeof(ARCHIVEFS_ALIAS_TARGET) - 1u)

static inline ArchiveFsAliasResult ArchiveFsParseAlias(const uint16_t *name, size_t n,
                                                        int include_unqualified,
                                                        ArchiveFsAlias *result)
{
    size_t begin = 1, server_end, host_begin, host_end, share, total;
    int qualified;
    if (!name || !result || n < 6 || n > 32767 || name[0] != '\\')
        return ArchiveFsAliasUnrelated;
    if (n > 12 && ArchiveFsAsciiEqual(name, 12, "\\device\\mup\\"))
        begin = 12;
    if (n - begin < 4 || !ArchiveFsAsciiEqual(name + begin, 4, "sfs."))
        return ArchiveFsAliasUnrelated;
    server_end = begin;
    while (server_end < n && name[server_end] != '\\') ++server_end;
    if (ArchiveFsAsciiEqual(name + begin, server_end - begin, "sfs.localhost"))
        return ArchiveFsAliasUnrelated;
    host_begin = begin + 4;
    host_end = server_end;
    qualified = host_end - host_begin > 10 &&
        ArchiveFsAsciiEqual(name + host_end - 10, 10, ".localhost");
    if (qualified) host_end -= 10;
    else if (!include_unqualified) return ArchiveFsAliasUnrelated;
    if (!ArchiveFsHostValid(name + host_begin, host_end - host_begin) ||
        server_end == n || server_end + 1 == n)
        return ArchiveFsAliasInvalid;
    share = server_end + 1;
    if (!ArchiveFsPathValid(name + share, n - share))
        return ArchiveFsAliasInvalid;
    total = ARCHIVEFS_ALIAS_TARGET_LENGTH + (host_end - host_begin) + 1 + (n - share);
    if (total > 32767) return ArchiveFsAliasInvalid;
    result->host_begin = host_begin;
    result->host_length = host_end - host_begin;
    result->share_begin = share;
    result->target_length = total;
    return ArchiveFsAliasMatched;
}

static inline void ArchiveFsMakeAlias(const uint16_t *name, size_t n,
                                      const ArchiveFsAlias *alias, uint16_t *target)
{
    size_t i, pos = 0;
    const char prefix[] = ARCHIVEFS_ALIAS_TARGET;
    for (i = 0; i < sizeof(prefix) - 1; ++i) target[pos++] = (uint16_t)(unsigned char)prefix[i];
    for (i = 0; i < alias->host_length; ++i) target[pos++] = name[alias->host_begin + i];
    target[pos++] = '\\';
    for (i = alias->share_begin; i < n; ++i) target[pos++] = name[i];
}
#endif
