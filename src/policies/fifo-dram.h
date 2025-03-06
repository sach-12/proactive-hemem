#ifndef FIFO_DRAM_H
#define FIFO_DRAM_H

#include <stdint.h>
#include <stdbool.h>

#include "../hemem.h"
#include "paging.h"

struct hemem_page* fifo_pagefault(void);
void fifo_init(void);
void fifo_remove_page(struct hemem_page *page);
void fifo_stats();

#endif // HEMEM_SIMPLE_H