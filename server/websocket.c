/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2015 Jeremy White

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#define _GNU_SOURCE
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <sys/types.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <glib.h>

#include <common/log.h>
#include <common/mem.h>

#include "sys-socket.h"
#include "websocket.h"

#ifdef _WIN32
#include <shlwapi.h>
#define strcasestr(haystack, needle) StrStrIA(haystack, needle)
#endif

/* Constants / masks all from RFC 6455 */

#define FIN_FLAG        0x80
#define RSV_MASK        0x70
#define TYPE_MASK       0x0F
#define CONTROL_FRAME_MASK 0x8

#define CONTINUATION_FRAME  0x0
#define TEXT_FRAME      0x1
#define BINARY_FRAME    0x2
#define CLOSE_FRAME     0x8
#define PING_FRAME      0x9
#define PONG_FRAME      0xA

#define LENGTH_MASK     0x7F
#define LENGTH_16BIT    0x7E
#define LENGTH_64BIT    0x7F

#define MASK_FLAG       0x80

#define WEBSOCKET_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define WEBSOCKET_MAX_HEADER_SIZE (1 + 9 + 4)

typedef struct {
    uint8_t type;
    uint8_t header[WEBSOCKET_MAX_HEADER_SIZE];
    int header_pos;
    bool frame_ready:1;
    bool masked:1;
    uint8_t mask[4];
    uint64_t relayed;
    uint64_t expected_len;
} websocket_frame_t;

struct RedsWebSocket {
    bool closed;

    websocket_frame_t read_frame;
    uint64_t write_remainder;
    uint8_t write_header[WEBSOCKET_MAX_HEADER_SIZE];
    uint8_t write_header_pos, write_header_len;
    bool close_pending;

    void *raw_stream;
    websocket_read_cb_t raw_read;
    websocket_write_cb_t raw_write;
    websocket_writev_cb_t raw_writev;
};

static int websocket_ack_close(RedsWebSocket *ws);
static int send_pending_data(RedsWebSocket *ws);

/* Perform a case insensitive search for needle in haystack.
   If found, return a pointer to the byte after the end of needle.
   Otherwise, return NULL */
static const char *find_str(const char *haystack, const char *needle)
{
    const char *s = strcasestr(haystack, needle);

    if (s) {
        return s + strlen(needle);
    }
    return NULL;
}

/* Extract WebSocket style length. Returns 0 if not enough data present,
   Always updates the output 'used' variable to the number of bytes
   required to extract the length; useful for tracking where the
   mask will be.
*/
static uint64_t extract_length(const uint8_t *buf, int *used)
{
    int i;
    uint64_t outlen = (*buf++) & LENGTH_MASK;

    (*used)++;

    switch (outlen) {
    case LENGTH_64BIT:
        *used += 8;
        outlen = 0;
        for (i = 56; i >= 0; i -= 8) {
            outlen |= (*buf++) << i;
        }
        break;

    case LENGTH_16BIT:
        *used += 2;
        outlen = ((*buf) << 8) | *(buf + 1);
        break;

    default:
        break;
    }
    return outlen;
}

static int frame_bytes_needed(websocket_frame_t *frame)
{
    int needed = 2;
    if (frame->header_pos < needed) {
        return needed - frame->header_pos;
    }

    switch (frame->header[1] & LENGTH_MASK) {
    case LENGTH_64BIT:
        needed += 8;
        break;
    case LENGTH_16BIT:
        needed += 2;
        break;
    }

    if (frame->header[1] & MASK_FLAG) {
        needed += 4;
    }

    return needed - frame->header_pos;
}

/*
* Generate WebSocket style response key, based on the
*  original key sent to us
* If non null, caller must free returned key string.
*/
static char *generate_reply_key(char *buf)
{
    GChecksum *checksum;
    char *b64 = NULL;
    uint8_t *sha1;
    size_t sha1_size;
    const char *key;
    const char *p;
    char *k;

    key = find_str(buf, "\nSec-WebSocket-Key:");
    if (key) {
        p = strchr(key, '\r');
        if (p) {
            k = g_strndup(key, p - key);
            k = g_strstrip(k);
            checksum = g_checksum_new(G_CHECKSUM_SHA1);
            g_checksum_update(checksum, (uint8_t *) k, strlen(k));
            g_checksum_update(checksum, (uint8_t *) WEBSOCKET_GUID, strlen(WEBSOCKET_GUID));
            g_free(k);

            sha1_size = g_checksum_type_get_length(G_CHECKSUM_SHA1);
            sha1 = g_malloc(sha1_size);

            g_checksum_get_digest(checksum, sha1, &sha1_size);

            b64 = g_base64_encode(sha1, sha1_size);

            g_checksum_free(checksum);
            g_free(sha1);
        }
    }

    return b64;
}


static void websocket_clear_frame(websocket_frame_t *frame)
{
    memset(frame, 0, sizeof(*frame));
}

/* Extract a frame header of data from a set of data transmitted by
    a WebSocket client. Returns success or error */
static bool websocket_get_frame_header(websocket_frame_t *frame)
{
    int fin;
    int used = 0;

    if (frame_bytes_needed(frame) > 0) {
        return true;
    }

    fin = frame->header[0] & FIN_FLAG;
    frame->type = frame->header[0] & TYPE_MASK;
    used++;

    // reserved bits are not expected
    if (frame->header[0] & RSV_MASK) {
        return false;
    }
    // control commands cannot be split
    if (!fin && (frame->type & CONTROL_FRAME_MASK) != 0) {
        return false;
    }
    if ((frame->type & ~CONTROL_FRAME_MASK) >= 3) {
        return false;
    }

    frame->masked = !!(frame->header[1] & MASK_FLAG);

    /* This is a Spice specific optimization.  We don't really
       care about assembling frames fully, so we treat
       a frame in process as a finished frame and pass it along. */
    if (!fin && frame->type == CONTINUATION_FRAME) {
        frame->type = BINARY_FRAME;
    }

    frame->expected_len = extract_length(frame->header + used, &used);

    if (frame->masked) {
        memcpy(frame->mask, frame->header + used, 4);
    }

    /* control frames cannot have more than 125 bytes of data */
    if ((frame->type & CONTROL_FRAME_MASK) != 0 &&
        frame->expected_len >= LENGTH_16BIT) {
        return false;
    }

    frame->relayed = 0;
    frame->frame_ready = true;
    return true;
}

static int relay_data(uint8_t* buf, size_t size, websocket_frame_t *frame)
{
    int i;
    int n = MIN(size, frame->expected_len - frame->relayed);

    if (frame->masked) {
        for (i = 0; i < n; i++, frame->relayed++) {
            *buf++ ^= frame->mask[frame->relayed % 4];
        }
    }

    return n;
}

int websocket_read(RedsWebSocket *ws, uint8_t *buf, size_t size)
{
    int n = 0;
    int rc;
    websocket_frame_t *frame = &ws->read_frame;

    if (ws->closed || ws->close_pending) {
        /* this avoids infinite loop in the case connection is still open and we have
         * pending data */
        uint8_t discard[128];
        ws->raw_read(ws->raw_stream, discard, sizeof(discard));
        return 0;
    }

    while (size > 0) {
        // make sure we have a proper frame ready
        if (!frame->frame_ready) {
            rc = ws->raw_read(ws->raw_stream, frame->header + frame->header_pos,
                              frame_bytes_needed(frame));
            if (rc <= 0) {
                goto read_error;
            }
            frame->header_pos += rc;

            if (!websocket_get_frame_header(frame)) {
                ws->closed = true;
                errno = EIO;
                return -1;
            }
        } else if (frame->type == CLOSE_FRAME) {
            ws->close_pending = true;
            websocket_clear_frame(frame);
            send_pending_data(ws);
            return 0;
        } else if (frame->type == BINARY_FRAME) {
            rc = ws->raw_read(ws->raw_stream, buf,
                              MIN(size, frame->expected_len - frame->relayed));
            if (rc <= 0) {
                goto read_error;
            }

            rc = relay_data(buf, rc, frame);
            n += rc;
            buf += rc;
            size -= rc;
            if (frame->relayed >= frame->expected_len) {
                websocket_clear_frame(frame);
            }
        } else {
            /* TODO - We don't handle PING at this point */
            spice_warning("Unexpected WebSocket frame.type %d.  Failure now likely.", frame->type);
            websocket_clear_frame(frame);
            continue;
        }
    }

    return n;

read_error:
    if (n > 0 && rc == -1 && (errno == EINTR || errno == EAGAIN)) {
        return n;
    }
    if (rc == 0) {
        ws->closed = true;
    }
    return rc;
}

static int fill_header(uint8_t *header, uint64_t len)
{
    int used = 0;
    int i;

    header[0] = FIN_FLAG | BINARY_FRAME;
    used++;

    header[1] = 0;
    used++;
    if (len > 65535) {
        header[1] |= LENGTH_64BIT;
        for (i = 9; i >= 2; i--) {
            header[i] = len & 0xFF;
            len >>= 8;
        }
        used += 8;
    } else if (len >= LENGTH_16BIT) {
        header[1] |= LENGTH_16BIT;
        header[2] = len >> 8;
        header[3] = len & 0xFF;
        used += 2;
    } else {
        header[1] |= len;
    }

    return used;
}

static void constrain_iov(struct iovec *iov, int iovcnt,
                          struct iovec **iov_out, int *iov_out_cnt,
                          uint64_t maxlen)
{
    int i;

    for (i = 0; i < iovcnt && maxlen > 0; i++) {
        if (iov[i].iov_len > maxlen) {
            /* TODO - This code has never triggered afaik... */
            *iov_out_cnt = ++i;
            *iov_out = g_memdup(iov, i * sizeof (*iov));
            (*iov_out)[i-1].iov_len = maxlen;
            return;
        }
        maxlen -= iov[i].iov_len;
    }

    /* we must trim the iov in case maxlen initially matches some chunks
     * For instance if initially we had 2 chunks 256 and 128 bytes respectively
     * and a maxlen of 256 we should just return the first chunk */
    *iov_out_cnt = i;
    *iov_out = iov;
}

static int send_data_header_left(RedsWebSocket *ws)
{
    /* send the pending header */
    /* this can be tested capping the length with MIN with a small size like 3 */
    int rc = ws->raw_write(ws->raw_stream, ws->write_header + ws->write_header_pos,
                           ws->write_header_len - ws->write_header_pos);
    if (rc <= 0) {
        return rc;
    }
    ws->write_header_pos += rc;

    /* if header was sent now we can send data */
    if (ws->write_header_pos >= ws->write_header_len) {
        int used = 1;
        ws->write_remainder = extract_length(ws->write_header + used, &used);
        return ws->write_header_len;
    }

    /* otherwise try to send the rest later */
    errno = EAGAIN;
    return -1;
}

static int send_data_header(RedsWebSocket *ws, uint64_t len)
{
    spice_assert(ws->write_header_pos >= ws->write_header_len);
    spice_assert(ws->write_remainder == 0);

    /* fill a new header */
    ws->write_header_pos = 0;
    ws->write_header_len = fill_header(ws->write_header, len);

    return send_data_header_left(ws);
}

static int send_pending_data(RedsWebSocket *ws)
{
    int rc;

    /* don't send while we are sending a data frame */
    if (ws->write_remainder) {
        return 1;
    }

    /* write pending data frame header not send completely */
    if (ws->write_header_pos < ws->write_header_len) {
        rc = send_data_header_left(ws);
        if (rc <= 0) {
            return rc;
        }
        return 1;
    }

    /* write close frame */
    if (ws->close_pending) {
        rc = websocket_ack_close(ws);
        if (rc <= 0) {
            return rc;
        }
    }
    return 1;
}

/* Write a WebSocket frame with the enclosed data out. */
int websocket_writev(RedsWebSocket *ws, const struct iovec *iov, int iovcnt)
{
    uint64_t len;
    int rc;
    struct iovec *iov_out;
    int iov_out_cnt;
    int i;

    if (ws->closed) {
        errno = EPIPE;
        return -1;
    }
    rc = send_pending_data(ws);
    if (rc <= 0) {
        return rc;
    }
    if (ws->write_remainder > 0) {
        constrain_iov((struct iovec *) iov, iovcnt, &iov_out, &iov_out_cnt, ws->write_remainder);
        rc = ws->raw_writev(ws->raw_stream, iov_out, iov_out_cnt);
        if (iov_out != iov) {
            g_free(iov_out);
        }
        if (rc <= 0) {
            return rc;
        }
        ws->write_remainder -= rc;
        return rc;
    }

    iov_out_cnt = iovcnt + 1;
    iov_out = g_malloc(iov_out_cnt * sizeof(*iov_out));

    for (i = 0, len = 0; i < iovcnt; i++) {
        len += iov[i].iov_len;
        iov_out[i + 1] = iov[i];
    }

    ws->write_header_pos = 0;
    ws->write_header_len = fill_header(ws->write_header, len);
    iov_out[0].iov_len = ws->write_header_len;
    iov_out[0].iov_base = ws->write_header;
    rc = ws->raw_writev(ws->raw_stream, iov_out, iov_out_cnt);
    g_free(iov_out);
    if (rc <= 0) {
        ws->write_header_len = 0;
        return rc;
    }

    /* this can happen if we can't write the header */
    if (SPICE_UNLIKELY(rc < ws->write_header_len)) {
        ws->write_header_pos = ws->write_header_len - rc;
        errno = EAGAIN;
        return -1;
    }
    ws->write_header_pos = ws->write_header_len;
    rc -= ws->write_header_len;

    /* Key point:  if we did not write out all the data, remember how
       much more data the client is expecting, and write that data without
       a header of any kind the next time around */
    ws->write_remainder = len - rc;

    return rc;
}

int websocket_write(RedsWebSocket *ws, const void *buf, size_t len)
{
    int rc;

    if (ws->closed) {
        errno = EPIPE;
        return -1;
    }

    rc = send_pending_data(ws);
    if (rc <= 0) {
        return rc;
    }
    if (ws->write_remainder == 0) {
        rc = send_data_header(ws, len);
        if (rc <= 0) {
            return rc;
        }
        len = ws->write_remainder;
    } else {
        len = MIN(ws->write_remainder, len);
    }

    rc = ws->raw_write(ws->raw_stream, buf, len);
    if (rc > 0) {
        ws->write_remainder -= rc;
    }
    return rc;
}

static int websocket_ack_close(RedsWebSocket *ws)
{
    unsigned char header[2];
    int rc;

    header[0] = FIN_FLAG | CLOSE_FRAME;
    header[1] = 0;

    rc = ws->raw_write(ws->raw_stream, header, sizeof(header));
    if (rc == sizeof(header)) {
        ws->close_pending = false;
        ws->closed = true;
    }
    return rc;
}

static bool websocket_is_start(char *buf)
{
    const char *protocol = find_str(buf, "\nSec-WebSocket-Protocol:");
    const char *key = find_str(buf, "\nSec-WebSocket-Key:");

    if (strncmp(buf, "GET ", 4) != 0 ||
            protocol == NULL || key == NULL ||
            !g_str_has_suffix(buf, "\r\n\r\n")) {
        return false;
    }

    /* check protocol value ignoring spaces before and after */
    int binary_pos = -1;
    sscanf(protocol, " binary %n", &binary_pos);
    if (binary_pos <= 0) {
        return false;
    }

    return true;
}

static void websocket_create_reply(char *buf, char *outbuf)
{
    char *key;

    key = generate_reply_key(buf);
    sprintf(outbuf, "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: %s\r\n"
                    "Sec-WebSocket-Protocol: binary\r\n\r\n", key);
    g_free(key);
}

RedsWebSocket *websocket_new(const void *buf, size_t len, void *stream, websocket_read_cb_t read_cb,
                             websocket_write_cb_t write_cb, websocket_writev_cb_t writev_cb)
{
    char rbuf[4096];

    memcpy(rbuf, buf, len);
    int rc = read_cb(stream, rbuf + len, sizeof(rbuf) - len - 1);
    if (rc <= 0) {
        return NULL;
    }
    len += rc;
    rbuf[len] = 0;

    /* TODO:  this has a theoretical flaw around packet buffering
              that is not likely to occur in practice.  That is,
              to be fully correct, we should repeatedly read bytes until
              either we get the end of the GET header (\r\n\r\n), or until
              an amount of time has passed.  Instead, we just read for
              16 bytes, and then read up to the sizeof rbuf.  So if the
              GET request is only partially complete at this point we
              will fail.

              A typical GET request is 520 bytes, and it's difficult to
              imagine a real world case where that will come in fragmented
              such that we trigger this failure.  Further, the spice reds
              code has no real mechanism to do variable length/time based reads,
              so it seems wisest to live with this theoretical flaw.
    */

    if (!websocket_is_start(rbuf)) {
        return NULL;
    }

    char outbuf[1024];

    websocket_create_reply(rbuf, outbuf);
    rc = write_cb(stream, outbuf, strlen(outbuf));
    if (rc != strlen(outbuf)) {
        return NULL;
    }

    RedsWebSocket *ws = g_new0(RedsWebSocket, 1);

    ws->raw_stream = stream;
    ws->raw_read = read_cb;
    ws->raw_write = write_cb;
    ws->raw_writev = writev_cb;

    return ws;
}

void websocket_free(RedsWebSocket *ws)
{
    g_free(ws);
}