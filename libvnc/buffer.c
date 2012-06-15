#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buffer.h"


void buffer_reserve(Buffer *buffer, size_t len)
{
    if ((buffer->capacity - buffer->offset) < len) {
	buffer->capacity += (len + 1024);
	buffer->buffer = realloc(buffer->buffer, buffer->capacity);
	if (buffer->buffer == NULL) {
	    fprintf(stderr, "vnc: out of memory\n");
	    exit(1);
	}
    }
}

int buffer_empty(Buffer *buffer)
{
    return buffer->offset == 0;
}

uint8_t *buffer_end(Buffer *buffer)
{
    return buffer->buffer + buffer->offset;
}

void buffer_reset(Buffer *buffer)
{
    buffer->offset = 0;
}

void buffer_append(Buffer *buffer, const void *data, size_t len)
{
    if (buffer->offset + len > buffer->capacity) {
	fprintf(stderr, "vnc: buffer overflow prevented\n");
	exit(1);
    }
    memcpy(buffer->buffer + buffer->offset, data, len);
    buffer->offset += len;
}

