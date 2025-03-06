/*
 * =====================================================================================
 *
 *       Filename:  fifo-dram.c
 *
 *    Description:  This is the fifo policy in the dram. It allocates pages in the DRAM
 *                  until it gets full then it moves pages to NVM based on FIFO policy
 *                  and places new pages in DRAM.
 *
 * =====================================================================================
 */
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <sys/time.h>

#include "../hemem.h"
#include "fifo-dram.h"
#include "../timer.h"
#include "../fifo.h"

uint64_t fastmem = 0;
uint64_t slowmem = 0;
bool slowmem_switch = false;

static struct fifo_list dram_free, nvm_free, dram_active, nvm_active;

void fifo_remove_page(struct hemem_page *page)
{
  if (page->in_dram) {
    page->present = false;
    page_list_remove_page(&dram_active, page);
    enqueue_fifo(&dram_free, page);
    fastmem -= PAGE_SIZE;
  }
  else {
    page->present = false;
    page_list_remove_page(&nvm_active, page);
    enqueue_fifo(&nvm_free, page);
    slowmem -= PAGE_SIZE;
  }
}

struct hemem_page* fifo_pagefault(void)
{
  struct timeval start, end;
  struct hemem_page *page, *move_out_page;

  gettimeofday(&start, NULL);

  page = dequeue_fifo(&dram_free);
  if (page != NULL) {
    assert(!page->present);
    page->present = true;
    fastmem += PAGE_SIZE; 
    enqueue_fifo(&dram_active, page) 
  }
  else {
    assert(slowmem < nvmsize);
    page = dequeue_fifo(&nvm_free);
    
    assert(page != NULL);
    assert(!page->present);

    page->present = true;
    slowmem += PAGE_SIZE;
    move_out_page = dequeue_fifo(&dram_active);
    enqueue_fifo(&dram_active, page)
    enqueue_fifo(&nvm_active, move_out_page)
  }
  gettimeofday(&end, NULL);
  LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));
  
  return page;
}

void fifo_init(void)
{
  pthread_mutex_init(&(dram_free.list_lock), NULL);
  for (int i = 0; i < dramsize / PAGE_SIZE; i++) {
    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    p->devdax_offset = i * PAGE_SIZE + dramoffset;
    p->present = false;
    p->in_dram = true;
    p->pt = pagesize_to_pt(PAGE_SIZE);
    pthread_mutex_init(&(p->page_lock), NULL);
    enqueue_fifo(&dram_free, p);
  }

  pthread_mutex_init(&(nvm_free.list_lock), NULL);
  for (int i = 0; i < nvmsize / PAGE_SIZE; i++) {
    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    p->devdax_offset = i * PAGE_SIZE + nvmoffset;
    p->present = false;
    p->in_dram = false;
    p->pt = pagesize_to_pt(PAGE_SIZE);
    pthread_mutex_init(&(p->page_lock), NULL);
    enqueue_fifo(&nvm_free, p);
  }
  LOG("Memory management policy is simple\n");
}

void fifo_stats()
{
  LOG_STATS("\tfastmem_allocated: [%ld]\tslowmem_allocated: [%ld]\n", fastmem, slowmem);
}
