#ifndef MYMALLOC_H
#define MYMALLOC_H

// extern int errno ;

#include "graphConfig.h"

char *strerror(int errnum);
void *aligned_malloc( size_t size );
void *my_malloc( size_t size );
void *regular_malloc( size_t size );

#endif