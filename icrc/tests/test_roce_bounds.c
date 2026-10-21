#include "roce.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned random_state = 12345;
static unsigned next(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}
int main(void)
{
    struct roce_destination dst = {{2, 0, 0, 0, 0, 1}, 0xc0000201, 101};
    /* Exact allocation boundaries let ASan catch reads beyond truncated input. */
    for (size_t length = 0; length < 512; ++length) {
        uint8_t *frame = malloc(length ? length : 1);
        uint8_t *saved = malloc(length ? length : 1);
        assert(frame && saved);
        for (unsigned trial = 0; trial < 32; ++trial) {
            for (size_t i = 0; i < length; ++i) frame[i] = next() >> 24;
            if (length >= 58 && trial % 2) {
                frame[12] = 8; frame[13] = 0; frame[14] = 0x45;
                frame[23] = 17; frame[20] = frame[21] = 0;
                frame[36] = 0x12; frame[37] = 0xb7;
                if (trial % 4 == 1) {
                    size_t ip_length = (length - 14) & ~(size_t)3;
                    frame[16] = ip_length >> 8; frame[17] = ip_length;
                    frame[38] = (ip_length - 20) >> 8; frame[39] = ip_length - 20;
                    frame[42] = trial % 6; frame[43] = 0;
                }
            }
            memcpy(saved, frame, length);
            struct roce_packet parsed;
            (void)roce_parse(frame, length, &parsed);
            (void)roce_check_icrc(frame, length);
            int status = roce_rewrite(frame, length, &dst);
            if (status != ROCE_OK) assert(!memcmp(saved, frame, length));
            else assert(roce_check_icrc(frame, length) == ROCE_OK);
        }
        free(saved); free(frame);
    }
    return 0;
}
