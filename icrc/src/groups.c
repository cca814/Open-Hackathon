#include "groups.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_MAX (1024u * 1024u)
struct parser { const char *start, *p; };
static void whitespace(struct parser *p)
{
    while (*p->p == ' ' || *p->p == '\n' || *p->p == '\r' || *p->p == '\t')
        ++p->p;
}
static int take(struct parser *p, char c)
{
    whitespace(p);
    if (*p->p != c) return -1;
    ++p->p;
    return 0;
}
static int string(struct parser *p, char *out, size_t size)
{
    if (take(p, '"')) return -1;
    size_t n = 0;
    while (*p->p && *p->p != '"') {
        unsigned char c = (unsigned char)*p->p++;
        if (c < 32 || c > 126 || c == '\\' || n + 1 >= size) return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return take(p, '"');
}
static int number(struct parser *p, uint32_t *value)
{
    whitespace(p);
    if (*p->p < '0' || *p->p > '9') return -1;
    if (*p->p == '0' && p->p[1] >= '0' && p->p[1] <= '9') return -1;
    uint32_t n = 0;
    while (*p->p >= '0' && *p->p <= '9') {
        n = n * 10 + (unsigned)(*p->p++ - '0');
        if (n > 0xffffff) return -1;
    }
    *value = n;
    return 0;
}
static int address(struct parser *p, int family, struct roce_address *value)
{
    char text[INET6_ADDRSTRLEN];
    if (string(p, text, sizeof(text)) ||
        inet_pton(family == ROCE_IPV4 ? AF_INET : AF_INET6, text, value->bytes) != 1)
        return -1;
    value->family = (uint8_t)family;
    return 0;
}
static int address_equal(const struct roce_address *a, const struct roce_address *b)
{
    size_t length = a->family == ROCE_IPV4 ? 4 : 16;
    return a->family == b->family && !memcmp(a->bytes, b->bytes, length);
}
static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int mac(struct parser *p, uint8_t *bytes)
{
    char text[18];
    if (string(p, text, sizeof(text)) || strlen(text) != 17) return -1;
    for (unsigned i = 0; i < 6; ++i) {
        int a = hex(text[3 * i]), b = hex(text[3 * i + 1]);
        if (a < 0 || b < 0 || (i < 5 && text[3 * i + 2] != ':')) return -1;
        bytes[i] = (uint8_t)(a * 16 + b);
    }
    return 0;
}
static int endpoint(struct parser *p, struct roce_destination *dst, int member)
{
    if (take(p, '{')) return -1;
    unsigned seen = 0;
    for (;;) {
        char key[32]; unsigned bit;
        if (string(p, key, sizeof(key)) || take(p, ':')) return -1;
        if (!strcmp(key, "dst_ipv4")) { bit = 1; if (address(p, ROCE_IPV4, &dst->address)) return -1; }
        else if (!strcmp(key, "dst_ipv6")) { bit = 1; if (address(p, ROCE_IPV6, &dst->address)) return -1; }
        else if (!strcmp(key, "dst_qpn")) { bit = 2; if (number(p, &dst->qpn)) return -1; }
        else if (member && !strcmp(key, "dst_mac")) { bit = 4; if (mac(p, dst->mac)) return -1; }
        else return -1;
        if (seen & bit) return -1;
        seen |= bit;
        whitespace(p);
        if (*p->p == '}') { ++p->p; break; }
        if (take(p, ',')) return -1;
    }
    return seen == (member ? 7u : 3u) ? 0 : -1;
}
static int members(struct parser *p, struct groups *table, struct multicast_group *group)
{
    if (take(p, '[')) return -1;
    group->first_member = table->member_count;
    for (;;) {
        if (group->member_count == GROUP_MEMBERS_MAX || table->member_count == MEMBERS_MAX) return -1;
        struct roce_destination dst = {0};
        if (endpoint(p, &dst, 1)) return -1;
        for (size_t i = group->first_member; i < table->member_count; ++i) {
            const struct roce_destination *other = &table->member[i];
            if (address_equal(&other->address, &dst.address) && other->qpn == dst.qpn &&
                !memcmp(other->mac, dst.mac, 6)) return -1;
        }
        table->member[table->member_count++] = dst;
        ++group->member_count;
        whitespace(p);
        if (*p->p == ']') { ++p->p; return 0; }
        if (take(p, ',')) return -1;
    }
}
static int group(struct parser *p, struct groups *table)
{
    if (take(p, '{') || table->count == GROUPS_MAX) return -1;
    struct multicast_group *g = &table->group[table->count];
    unsigned seen = 0;
    for (;;) {
        char key[32]; unsigned bit;
        if (string(p, key, sizeof(key)) || take(p, ':')) return -1;
        if (!strcmp(key, "id")) {
            bit = 1;
            if (string(p, g->id, sizeof(g->id)) || !g->id[0]) return -1;
        } else if (!strcmp(key, "match")) {
            bit = 2; struct roce_destination match = {0};
            if (endpoint(p, &match, 0)) return -1;
            g->destination = match.address; g->dst_qpn = match.qpn;
        } else if (!strcmp(key, "members")) {
            bit = 4; if (seen & bit || members(p, table, g)) return -1;
        } else return -1;
        if (seen & bit) return -1;
        seen |= bit;
        whitespace(p);
        if (*p->p == '}') { ++p->p; break; }
        if (take(p, ',')) return -1;
    }
    if (seen != 7) return -1;
    for (size_t i = g->first_member; i < g->first_member + g->member_count; ++i)
        if (table->member[i].address.family != g->destination.family)
            return -1;
    for (size_t i = 0; i < table->count; ++i)
        if (!strcmp(g->id, table->group[i].id) ||
            (address_equal(&g->destination, &table->group[i].destination) &&
             g->dst_qpn == table->group[i].dst_qpn)) return -1;
    ++table->count;
    return 0;
}
static int parse(struct parser *p, struct groups *table)
{
    char key[16];
    if (take(p, '{') || string(p, key, sizeof(key)) || strcmp(key, "groups") ||
        take(p, ':') || take(p, '[')) return -1;
    for (;;) {
        if (group(p, table)) return -1;
        whitespace(p);
        if (*p->p == ']') { ++p->p; break; }
        if (take(p, ',')) return -1;
    }
    if (take(p, '}')) return -1;
    whitespace(p);
    return *p->p == 0 ? 0 : -1;
}

int groups_load(const char *path, struct groups **out, char *error, size_t size)
{
    if (!path || !out) return -1;
    FILE *file = fopen(path, "rb");
    if (!file) { if (error && size) snprintf(error, size, "Cannot open group file: %s", path); return -1; }
    char *text = malloc(FILE_MAX + 1);
    struct groups *table = calloc(1, sizeof(*table));
    int status = -1;
    if (!text || !table) {
        if (error && size) snprintf(error, size, "Group allocation failed");
        goto done;
    }
    size_t n = fread(text, 1, FILE_MAX, file);
    if (ferror(file) || n == FILE_MAX || memchr(text, 0, n)) {
        if (error && size) snprintf(error, size, "Unreadable, oversized or NUL-containing group file");
        goto done;
    }
    text[n] = 0;
    struct parser p = {text, text};
    if (parse(&p, table)) {
        if (error && size) snprintf(error, size, "Invalid group configuration near byte %zu (schema, duplicate or limit)", (size_t)(p.p - p.start));
        goto done;
    }
    *out = table;
    table = NULL;
    status = 0;
done:
    fclose(file); free(text); free(table);
    return status;
}

const struct multicast_group *groups_find(const struct groups *table,
                                          const struct roce_address *address,
                                          uint32_t qpn)
{
    if (table && address)
        for (size_t i = 0; i < table->count; ++i)
            if (address_equal(&table->group[i].destination, address) &&
                table->group[i].dst_qpn == qpn)
                return &table->group[i];
    return NULL;
}
