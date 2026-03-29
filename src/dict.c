#include "dict.h"
#include <stdlib.h>

#define DICT_INITIAL_SIZE 4

dictht *dictCreate(void)
{
    dictht *dict = malloc(sizeof(*dict));
    dict->size = DICT_INITIAL_SIZE;
    dict->sizemask = dict->size - 1;
    dict->used = 0;
    dict->table = calloc(dict->size, sizeof(dictEntry *));
    return dict;
}
