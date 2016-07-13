/*
 * Driver for the Plantower PMS3003 and PMS5003.
 *
 * Licensed under the Apache License, Version 2.0, January 2004 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *      http://www.apache.org/licenses/
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS WITH THE SOFTWARE.
 *
 */

#include <stdint.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <esp/uart.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"

#include "buffer.h"
#include "leds.h"



static uint8_t mygetc()
{
    uint8_t ch;
    while (1) {
        if (read(0, (void*)&ch, 1)) {
            return ch;
        }
    }
}


/*
 * Variable bit length encoding support.
 */
static uint8_t outbuf[256];
static int noutbits;
static uint32_t outbits;
static int outlen;

static void emitbits(uint32_t bits, uint8_t nbits)
{
    outbits |= bits << noutbits;
    noutbits += nbits;
    while (noutbits >= 8) {
        outbuf[outlen++] = (unsigned char) (outbits & 0xFF);
        outbits >>= 8;
        noutbits -= 8;
    }
}

static void init_outbuf()
{
    outlen = 0;
    noutbits = 0;
    outbits = 0;
}

/*
 * Variable length encoded value for the PMS*003 events.
 */

static void emit_var_int(int32_t v)
{
    if (v == 0) {
        /* 0 -> '1' */
        emitbits(1, 1);
        return;
    }

    emitbits(0, 1);

    /* The sign bit. */
    if (v < 0) {
        emitbits(1, 1);
        v = -v;
    } else {
        emitbits(0, 1);
    }

    if (v == 1) {
        /* +1 -> '001'
         * -1 -> '011'
         */
        emitbits(1, 1);
        return;
    }

    emitbits(0, 1);

    if (v < 33) {
        /* +2 to +32 -> '000 xxxxx'
         * -2 to -32 -> '010 xxxxx'
         */
        emitbits(v - 2, 5);
        return;
    }
    
    emitbits(0x1f, 5);
        
    /* 16 bit unsigned value:
     *  +33 to 65568 : #x000 11111 xxxx xxxx xxxx xxxx
     *  -33 to 65568 : #x010 11111 xxxx xxxx xxxx xxxx
     */
    v = v - 33;
    emitbits(v & 0xffff, 16);
}

/*
 * The particle counter events are compressed. The prior event values are noted
 * here to support delta encoding, and initialized to zeros at the start of each
 * new data buffer so that each buffer can be decoded on its own.
 */

static void pms_read_task(void *pvParameters)
{
    /* Delta encoding state. */
    uint32_t last_index = 0;
    int32_t last_pm1a = 0;
    int32_t last_pm25ad = 0;
    int32_t last_pm10ad = 0;
    int32_t last_pm1b = 0;
    int32_t last_pm25bd = 0;
    int32_t last_pm10bd = 0;
    int32_t last_c1d = 0;
    int32_t last_c2d = 0;
    int32_t last_c3d = 0;
    int32_t last_c4d = 0;
    int32_t last_c5d = 0;
    int32_t last_c6 = 0;
    int32_t last_r1 = 0;

    for (;;) {
        /* Search for the "BM" header. */
        uint8_t c = mygetc();
        if (c != 'B')
            continue;
        
        c = mygetc();
        if (c != 'M')
            continue;

        /* Read the length. */
        uint16_t msb = mygetc();
        uint8_t lsb = mygetc();
        uint16_t length = msb << 8 | lsb;
        if (length != 0x14 && length != 0x1c)
            continue;

        uint16_t checksum = 143 + msb + lsb;

        msb = mygetc();
        lsb = mygetc();
        checksum += msb + lsb;
        int32_t pm1a = msb << 8 | lsb;

        msb = mygetc();
        lsb = mygetc();
        checksum += msb + lsb;
        int32_t pm25a = msb << 8 | lsb;
        int32_t pm25ad = pm25a - pm1a;

        msb = mygetc();
        lsb = mygetc();
        checksum += msb + lsb;
        int32_t pm10a = msb << 8 | lsb;
        int32_t pm10ad = pm10a - pm25a;

        msb = mygetc();
        lsb = mygetc();
        checksum += msb + lsb;
        int32_t pm1b = msb << 8 | lsb;

        msb = mygetc();
        lsb = mygetc();
        checksum += msb + lsb;
        int32_t pm25b = msb << 8 | lsb;
        int32_t pm25bd = pm25b - pm1b;

        msb = mygetc();
        lsb = mygetc();
        checksum += msb + lsb;
        int32_t pm10b = msb << 8 | lsb;
        int32_t pm10bd = pm10b - pm25b;

        msb = mygetc();
        lsb = mygetc();
        checksum += msb + lsb;
        int32_t c1 = msb << 8 | lsb;

        msb = mygetc();
        lsb = mygetc();
        checksum += msb + lsb;
        int32_t c2 = msb << 8 | lsb;

        int32_t c1d = c1 - c2;

        int32_t c3 = 0;
        int32_t c4 = 0;
        int32_t c5 = 0;
        int32_t c6 = 0;

        if (length == 0x1c) {
            msb = mygetc();
            lsb = mygetc();
            checksum += msb + lsb;
            c3 = msb << 8 | lsb;

            msb = mygetc();
            lsb = mygetc();
            checksum += msb + lsb;
            c4 = msb << 8 | lsb;

            msb = mygetc();
            lsb = mygetc();
            checksum += msb + lsb;
            c5 = msb << 8 | lsb;

            msb = mygetc();
            lsb = mygetc();
            checksum += msb + lsb;
            c6 = msb << 8 | lsb;
        }

        int32_t c2d = c2 - c3;

        int32_t c3d = 0;
        int32_t c4d = 0;
        int32_t c5d = 0;
        
        if (length == 0x1c) {
            c3d = c3 - c4;
            c4d = c4 - c5;
            c5d = c5 - c6;
        }

        msb = mygetc();
        lsb = mygetc();
        checksum += msb + lsb;
        int32_t r1 = msb << 8 | lsb;

        msb = mygetc();
        lsb = mygetc();
        uint16_t expected_checksum = msb << 8 | lsb;

        if (checksum != expected_checksum) {
            blink_red();
            continue;
        }

        while (1) {
            /* Variable length encoding. */
            init_outbuf();
            emit_var_int(pm1a - last_pm1a);
            emit_var_int(pm25ad - last_pm25ad);
            emit_var_int(pm10ad - last_pm10ad);
            emit_var_int(pm1b - last_pm1b);
            emit_var_int(pm25bd - last_pm25bd);
            emit_var_int(pm10bd - last_pm10bd);
            emit_var_int(c1d - last_c1d);
            emit_var_int(c2d - last_c2d);
            if (length == 0x1c) {
                emit_var_int(c3d - last_c3d);
                emit_var_int(c4d - last_c4d);
                emit_var_int(c5d - last_c5d);
                emit_var_int(c6 - last_c6);
            }
            emit_var_int(r1 - last_r1);
            
            /* Emit at least eight bits of the device supplied checksum and fill
             * to a byte boundary with the rest so there will always be at least
             * eight checksum bits and often more and at most 15 bits. */
            emitbits(expected_checksum, 15);

            int len = outlen;
            int32_t code = length == 0x14 ? DBUF_EVENT_PMS3003 : DBUF_EVENT_PMS5003;
            uint32_t new_index = dbuf_append(last_index, code, outbuf, len, 1, 0);
            if (new_index == last_index)
                break;

            /* Moved on to a new buffer. Reset the delta encoding state and
             * retry. */
            last_index = new_index;
            last_pm1a = 0;
            last_pm25ad = 0;
            last_pm10ad = 0;
            last_pm1b = 0;
            last_pm25bd = 0;
            last_pm10bd = 0;
            last_c1d = 0;
            last_c2d = 0;
            last_c3d = 0;
            last_c4d = 0;
            last_c5d = 0;
            last_c6 = 0;
            last_r1 = 0;
        };

        blink_green();
        
        /* Commit the values logged. Note this is the only task accessing this
         * state so these updates are synchronized with the last event of this
         * class append. */
        last_pm1a = pm1a;
        last_pm25ad = pm25ad;
        last_pm10ad = pm10ad;
        last_pm1b = pm1b;
        last_pm25bd = pm25bd;
        last_pm10bd = pm10bd;
        last_c1d = c1d;
        last_c2d = c2d;
        last_c3d = c3d;
        last_c4d = c4d;
        last_c5d = c5d;
        last_c6 = c6;
        last_r1 = r1;
    }
}

static void swap_uart0_pins(int swapped)
{
    if (swapped) {
        iomux_set_pullup_flags(3, 0);
        iomux_set_function(3, 4);
        iomux_set_pullup_flags(1, IOMUX_PIN_PULLUP);
        iomux_set_function(1, 4);
        DPORT.PERI_IO |= DPORT_PERI_IO_SWAP_UART0_PINS;
    } else {
        iomux_set_pullup_flags(5, 0);
        iomux_set_function(5, 0);
        iomux_set_pullup_flags(4, IOMUX_PIN_PULLUP);
        iomux_set_function(4, 0);
        DPORT.PERI_IO &= ~DPORT_PERI_IO_SWAP_UART0_PINS;
    }
}


void init_pms()
{
    xTaskCreate(&pms_read_task, (signed char *)"pms_read_task", 256, NULL, 3, NULL);

#if 0
    /* For the nodemcu board. */
    swap_uart0_pins(1);
#endif
}