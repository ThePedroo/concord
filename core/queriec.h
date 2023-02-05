#ifndef QUERIEC_H
#define QUERIEC_H

#define QUERIEC_ERROR_NOMEM -1
#define QUERIEC_OK 0

struct queriec_obj {
    int status;
    size_t size;
    size_t offset;
};

void
queriec_init(struct queriec_obj *obj, size_t size, size_t offset);

int queriec_snprintf_add(struct queriec_obj *obj, char *query, char key[], size_t keySize, 
                         char buffer[], size_t bufferLen, char *format, ...);

int
queriec_add(struct queriec_obj *obj, char *query, char key[], size_t keySize, char value[], size_t valueSize);

#endif