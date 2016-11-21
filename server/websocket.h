/*
 *  Copyright (C) 2015 Jeremy White
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

typedef ssize_t (*websocket_read_cb_t)(void *opaque, void *buf, size_t nbyte);
typedef ssize_t (*websocket_write_cb_t)(void *opaque, const void *buf, size_t nbyte);
typedef ssize_t (*websocket_writev_cb_t)(void *opaque, struct iovec *iov, int iovcnt);

typedef struct RedsWebSocket RedsWebSocket;

RedsWebSocket *websocket_new(const void *buf, size_t len, void *stream, websocket_read_cb_t read_cb,
                             websocket_write_cb_t write_cb, websocket_writev_cb_t writev_cb);
void websocket_free(RedsWebSocket *ws);
int websocket_read(RedsWebSocket *ws, uint8_t *buf, size_t len);
int websocket_write(RedsWebSocket *ws, const void *buf, size_t len);
int websocket_writev(RedsWebSocket *ws, const struct iovec *iov, int iovcnt);