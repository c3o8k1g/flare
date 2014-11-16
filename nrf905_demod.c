#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "lib_crc.h"

#define SAMPLE_RATE     (16)
#define PREAMBLE_BITS   (10)
#define PREAMBLE_SIZE   (2 * SAMPLE_RATE * PREAMBLE_BITS)
#define MAX_MSG_BYTES   (29)
#define MIN_MSG_SIZE    (2 * SAMPLE_RATE * 8 * CHAR_BIT)
#define BUFFER_SIZE     (1 << 13) /* must be power of 2, and larger than the expected message */

/*
 * nRF905 preamble is Manchester-encoded 0x3F6 (0xAAA66):
 * '10101010101001100110'
 */
const uint8_t hi_map[PREAMBLE_BITS] = { 1, 3, 5, 7, 9, 11, 12, 15, 16, 19 };
const uint8_t lo_map[PREAMBLE_BITS] = { 0, 2, 4, 6, 8, 10, 13, 14, 17, 18 };

/* Digital filter designed by mkfilter/mkshape/gencode   A.J. Fisher
   Command line: /www/usr/fisher/helpers/mkfilter -Bu -Lp -o 4 -a 1.0937500000e-01 0.0000000000e+00 -l */
__attribute__((always_inline)) double lowpass_i(double v) {
    static double xv[5], yv[5];

    xv[0] = xv[1]; xv[1] = xv[2]; xv[2] = xv[3]; xv[3] = xv[4];
    xv[4] = v / 1.530280502e+02;
    yv[0] = yv[1]; yv[1] = yv[2]; yv[2] = yv[3]; yv[3] = yv[4];
    yv[4] =   (xv[0] + xv[4]) + 4 * (xv[1] + xv[3]) + 6 * xv[2]
        + ( -0.1589997056 * yv[0]) + (  0.9185354694 * yv[1])
        + ( -2.0828312654 * yv[2]) + (  2.2187395070 * yv[3]);

    return yv[4];
}

__attribute__((always_inline)) double lowpass_q(double v) {
    static double xv[5], yv[5];

    xv[0] = xv[1]; xv[1] = xv[2]; xv[2] = xv[3]; xv[3] = xv[4];
    xv[4] = v / 1.530280502e+02;
    yv[0] = yv[1]; yv[1] = yv[2]; yv[2] = yv[3]; yv[3] = yv[4];
    yv[4] =   (xv[0] + xv[4]) + 4 * (xv[1] + xv[3]) + 6 * xv[2]
        + ( -0.1589997056 * yv[0]) + (  0.9185354694 * yv[1])
        + ( -2.0828312654 * yv[2]) + (  2.2187395070 * yv[3]);

    return yv[4];
}

/* Digital filter designed by mkfilter/mkshape/gencode   A.J. Fisher
   Command line: /www/usr/fisher/helpers/mkfilter -Bu -Lp -o 4 -a 6.2500000000e-02 0.0000000000e+00 -l */
__attribute__((always_inline)) double lowpass_d(double v) {
    static double xv[5], yv[5];

    xv[0] = xv[1]; xv[1] = xv[2]; xv[2] = xv[3]; xv[3] = xv[4];
    xv[4] = v / 1.071238871e+03;
    yv[0] = yv[1]; yv[1] = yv[2]; yv[2] = yv[3]; yv[3] = yv[4];
    yv[4] =   (xv[0] + xv[4]) + 4 * (xv[1] + xv[3]) + 6 * xv[2]
        + ( -0.3555773823 * yv[0]) + (  1.7861066002 * yv[1])
        + ( -3.4223095294 * yv[2]) + (  2.9768443337 * yv[3]);

    return yv[4];
}

void process_stream(FILE *stream) {
    uint8_t readbuf[BUFFER_SIZE << 1];
    uint16_t c;
    size_t len;

    static double i1, q1, i2, q2;
    double m, d;

    static int16_t buffer[BUFFER_SIZE]; // demodulated signal
    static int16_t m_buff[BUFFER_SIZE]; // magnitudes only; for RSSI estimation
    static uint16_t buffer_end = 0;
    static uint16_t buffer_skip = PREAMBLE_SIZE + 2 * SAMPLE_RATE * MAX_MSG_BYTES * CHAR_BIT;
    static int32_t sum = 0;
    #define BUF(i) (buffer[(buffer_end + i) & (BUFFER_SIZE - 1)])

    static uint8_t msg[MAX_MSG_BYTES];

    int16_t threshold;
    uint16_t i, j, k;
    uint8_t msg_len;
    uint8_t m1, m2;
    uint16_t crc16;

    double timestamp, rms;
    struct timeval tv;

    char out[128], *p;

    while (!feof(stream)) {
        len = fread(readbuf, sizeof(uint16_t), BUFFER_SIZE, stream);

        for (c = 0; c < len << 1; c += 2) {
            i1 = lowpass_i(readbuf[c + 0] - 127);
            q1 = lowpass_q(readbuf[c + 1] - 127);
            m = i1 * i1 + q1 * q1;
            d = (i1 * (q1 - q2) - q1 * (i1 - i2)) / (m + DBL_MIN);
            i2 = i1;
            q2 = q1;

            m_buff[buffer_end] = m;
            buffer[buffer_end] = lowpass_d(d * SHRT_MAX);
            buffer_end++;
            buffer_end &= BUFFER_SIZE - 1;

            sum -= BUF(BUFFER_SIZE);
            sum += BUF(PREAMBLE_SIZE + MIN_MSG_SIZE);

            if (buffer_skip) {
                buffer_skip--;
            } else {
                threshold = sum / (PREAMBLE_SIZE + MIN_MSG_SIZE);

                j = 0;
                for (i = 0; i < PREAMBLE_BITS; i++) {
                    if (BUF(hi_map[i] * SAMPLE_RATE) > threshold)
                        j++;
                    if (BUF(lo_map[i] * SAMPLE_RATE) < threshold)
                        j++;
                }

                if (j == 2 * PREAMBLE_BITS) {
                    memset(msg, 0, MAX_MSG_BYTES);
                    crc16 = 0xffff;
                    for (i = PREAMBLE_SIZE, j = 0; i < BUFFER_SIZE; i += SAMPLE_RATE * 2, j++) {
                        msg_len = j >> 3;
                        m1 = BUF(i) < threshold;
                        m2 = BUF(i + SAMPLE_RATE) < threshold;
                        if (m1 == m2)
                            break;
                        else if (m1)
                            msg[msg_len] |= 1 << ((CHAR_BIT - 1) - (j & (CHAR_BIT - 1)));

                        if ((j & (CHAR_BIT - 1)) == (CHAR_BIT - 1)) {
                            crc16 = update_crc_ccitt(crc16, msg[msg_len]);
                            if (crc16 == 0) {
                                msg_len++;

                                for (k = 0, p = out; k < msg_len; k++, p += 2)
                                    snprintf(p, 3, "%02x", msg[k]);

                                gettimeofday(&tv, NULL);
                                timestamp = tv.tv_sec + tv.tv_usec / 1e6;
                                timestamp -= (BUFFER_SIZE / SAMPLE_RATE) * 2e-5;

                                for (k = 0, rms = 0; k < i; k++) {
                                    m = sqrt(m_buff[(buffer_end + k) & (BUFFER_SIZE - 1)]);
                                    rms += m * m;
                                }
                                rms /= i;

                                snprintf(out + msg_len * 2, sizeof(out) + msg_len * 2,
                                        "\t%.06f\t%.01f",
                                        timestamp,
                                        20.0 * log10(sqrt(rms) / 181.019336)
                                );

                                puts(out);
                                fflush(stdout);

                                buffer_skip = PREAMBLE_SIZE + 2 * SAMPLE_RATE * msg_len * CHAR_BIT;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}

int main(int argc, char **argv) {
    process_stream(stdin);
    return 0;
}
