#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>

#include "queriec.h"

void
queriec_init(struct queriec_obj *obj, size_t size, size_t offset)
{
    obj->size = size;
    obj->status = 0;
    obj->offset = offset;
}

void
_queriec_add(struct queriec_obj *obj, char *query, char *letter, char key[], size_t keySize, char value[], size_t valueSize)
{
    size_t idx = obj->offset, i = 0;
        
    query[idx++] = letter[0];
    while (1) {
        if (i < keySize) query[idx++] = key[i++];
        else {
            idx--;
            break;
        }
    }
    i = 0;
    query[idx++] = '=';
    while (1) {
        if (i < valueSize) query[idx++] = value[i++];
        else break;
    }
}

int queriec_snprintf_add(struct queriec_obj *obj, char *query, char key[], size_t keySize, 
                         char buffer[], size_t bufferLen, char *format, ...)
{
    va_list args;
    va_start(args, format);
    size_t supposedSize = (size_t)vsnprintf(buffer, bufferLen, format, args);
    va_end(args);

    if (obj->offset + keySize + supposedSize + 2 > obj->size) return QUERIEC_ERROR_NOMEM;

    switch (obj->status) {
    case 0:
        _queriec_add(obj, query, "?", key, keySize, buffer, supposedSize);
        obj->status = 1;
        break;
    case 1:
        _queriec_add(obj, query, "&", key, keySize, buffer, supposedSize);
        break;
    }

    obj->offset += keySize + supposedSize + 2;

    return QUERIEC_OK;
}

int
queriec_add(struct queriec_obj *obj, char *query, char key[], size_t keySize, char value[], size_t valueSize)
{
    if (obj->offset + keySize + valueSize + 2 > obj->size) return QUERIEC_ERROR_NOMEM;

    switch (obj->status) {
    case 0:
        _queriec_add(obj, query, "?", key, keySize, value, valueSize);
        obj->status = 1;
        break;
    case 1:
        _queriec_add(obj, query, "&", key, keySize, value, valueSize);
        break;
    }

    obj->offset += keySize + valueSize + 2;

    return QUERIEC_OK;
}