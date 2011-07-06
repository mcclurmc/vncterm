#include <stddef.h>
#include <stdint.h>


typedef struct Buffer
{
    size_t capacity;
    size_t offset;
    uint8_t *buffer;
} Buffer;


void buffer_reserve(Buffer *buffer, size_t len);
int buffer_empty(Buffer *buffer);
uint8_t *buffer_end(Buffer *buffer);
void buffer_reset(Buffer *buffer);
void buffer_append(Buffer *buffer, const void *data, size_t len);
