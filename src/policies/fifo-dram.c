#define _GNU_SOURCE
#define HEMEM_INTERVAL 10000ULL
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <sys/time.h>
#include <unistd.h>
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/mman.h>
#include <sched.h>
#include <sys/ioctl.h>

#include "../hemem.h"
#include "fifo-dram.h"
#include "../timer.h"
#include "../spsc-ring.h"

uint64_t pebs_start_cpu;
uint64_t migration_thread_cpu;

static struct fifo_list dram_fifo_list;
static struct fifo_list nvm_fifo_list;
static struct fifo_list dram_free_list;
static struct fifo_list nvm_free_list;
static ring_handle_t free_page_ring;
static pthread_mutex_t free_page_ring_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t global_clock = 0;

uint64_t hemem_pages_cnt = 0;
uint64_t other_pages_cnt = 0;
uint64_t total_pages_cnt = 0;
uint64_t accesses_cnt[NPBUFTYPES];
uint64_t core_accesses_cnt[PEBS_NPROCS];
uint64_t zero_pages_cnt = 0;
uint64_t throttle_cnt = 0;
uint64_t unthrottle_cnt = 0;
uint64_t cools = 0;

_Atomic volatile double miss_ratio = -1.0;
FILE *miss_ratio_f = NULL;

static struct perf_event_mmap_page *perf_page[PEBS_NPROCS][NPBUFTYPES];
int pfd[PEBS_NPROCS][NPBUFTYPES];

volatile bool need_cool_dram = false;
volatile bool need_cool_nvm = false;

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
    int cpu, int group_fd, unsigned long flags)
{
  int ret;

  ret = syscall(__NR_perf_event_open, hw_event, pid, cpu,
                group_fd, flags);
  return ret;
}

static struct perf_event_mmap_page* perf_setup(__u64 config, __u64 config1, __u64 cpu, __u64 type)
{
  struct perf_event_attr attr;

  memset(&attr, 0, sizeof(struct perf_event_attr));

  attr.type = PERF_TYPE_RAW;
  attr.size = sizeof(struct perf_event_attr);

  attr.config = config;
  attr.config1 = config1;
  attr.sample_period = SAMPLE_PERIOD;

  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_WEIGHT | PERF_SAMPLE_ADDR;
  attr.disabled = 0;
  //attr.inherit = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.exclude_callchain_kernel = 1;
  attr.exclude_callchain_user = 1;
  attr.precise_ip = 1;

  pfd[cpu][type] = perf_event_open(&attr, -1, cpu, -1, 0);
  if(pfd[cpu][type] == -1) {
    perror("perf_event_open");
  }
  assert(pfd[cpu][type] != -1);

  fprintf(stderr, "Set up perf on core %llu\n", cpu);
  size_t mmap_size = sysconf(_SC_PAGESIZE) * PERF_PAGES;
  /* printf("mmap_size = %zu\n", mmap_size); */
  struct perf_event_mmap_page *p = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, pfd[cpu][type], 0);
  if(p == MAP_FAILED) {
    perror("mmap");
  }
  assert(p != MAP_FAILED);

  return p;
}

static void fifo_migrate_down(struct hemem_page *page, uint64_t offset)
{
  LOG("Migrate Down\n");
  struct timeval start, end;

  gettimeofday(&start, NULL);

  page->migrating = true;
  hemem_wp_page(page, true);
  hemem_migrate_down(page, offset);
  page->migrating = false;

  gettimeofday(&end, NULL);
  LOG_TIME("migrate_down: %f s\n", elapsed(&start, &end));
}

static void fifo_migrate_up(struct hemem_page *page, uint64_t offset)
{
  LOG("Migrate Up\n");
  struct timeval start, end;

  gettimeofday(&start, NULL);

  page->migrating = true;
  hemem_wp_page(page, true);
  hemem_migrate_up(page, offset);
  page->migrating = false;

  gettimeofday(&end, NULL);
  LOG_TIME("migrate_up: %f s\n", elapsed(&start, &end));
}

static struct hemem_page* start_dram_page = NULL;
static struct hemem_page* start_nvm_page = NULL;


static struct hemem_page* fifo_allocate_page() {
    struct timeval start, end;
    struct hemem_page *page, *evict_page, *new_nvm_page;
    LOG("In Allocate Page\n");
    gettimeofday(&start, NULL);

    // First, try to allocate a page in DRAM
    page = dequeue_fifo(&dram_free_list);
    if (page != NULL) {
        assert(page->in_dram);
        assert(!page->present);

        page->present = true;
        enqueue_fifo(&dram_fifo_list, page);

        gettimeofday(&end, NULL);
        LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

        return page;
    }

    // DRAM is full, evict the oldest DRAM page to NVM
    evict_page = dequeue_fifo(&dram_fifo_list);
    if (evict_page != NULL) {
        assert(evict_page->in_dram);

        new_nvm_page = dequeue_fifo(&nvm_free_list);
        if (new_nvm_page != NULL) {
            assert(!new_nvm_page->present);

            fifo_migrate_down(evict_page, new_nvm_page->devdax_offset);
            new_nvm_page->devdax_offset = evict_page->devdax_offset;
            new_nvm_page->in_dram = true;
            new_nvm_page->present = true;

            enqueue_fifo(&nvm_fifo_list, evict_page);
            enqueue_fifo(&dram_fifo_list, new_nvm_page);
            return new_nvm_page;
        } else {
            enqueue_fifo(&dram_fifo_list, evict_page);
            assert(!"Out of memory in NVM!");
        }
    }

    assert(!"Out of memory!");
}


struct hemem_page* fifo_pagefault(void)
{
  struct hemem_page *page;

  // do the heavy lifting of finding the devdax file offset to place the page
  page = fifo_allocate_page();
  assert(page != NULL);

  return page;
}

void fifo_remove_page(struct hemem_page *page)
{
  assert(page != NULL);

  LOG("fifo: remove page, put this page into free_page_ring: va: 0x%lx\n", page->va);

  pthread_mutex_lock(&free_page_ring_lock);
  while (ring_buf_full(free_page_ring));
  ring_buf_put(free_page_ring, (uint64_t*)page);
  pthread_mutex_unlock(&free_page_ring_lock);

  page->present = false;
  page->hot = false;
  for (int i = 0; i < NPBUFTYPES; i++) {
    page->accesses[i] = 0;
    page->tot_accesses[i] = 0;
  }
}

void fifo_init(void)
{
  pthread_t kswapd_thread;
  pthread_t scan_thread;
  uint64_t** buffer;
  char logpath[32];

  LOG("fifo_init: started\n");

  snprintf(&logpath[0], sizeof(logpath) - 1, "/tmp/log-%d.txt", getpid());
  miss_ratio_f = fopen(logpath, "w");
  if (miss_ratio_f == NULL) {
    perror("miss ratio file fopen");
  }
  assert(miss_ratio_f != NULL);

  char* pebs_start_cpu_string = getenv("PEBS_START_CPU");
  if(pebs_start_cpu_string != NULL)
    pebs_start_cpu = strtoull(pebs_start_cpu_string, NULL, 10);
  else
    pebs_start_cpu = START_THREAD_DEFAULT;

  //scanning_thread_cpu = hemem_start_cpu;
  migration_thread_cpu = hemem_start_cpu + 1;

  for (int i = pebs_start_cpu; i < pebs_start_cpu + num_cores; i++) {
    //perf_page[i][READ] = perf_setup(0x1cd, 0x4, i);  // MEM_TRANS_RETIRED.LOAD_LATENCY_GT_4
    //perf_page[i][READ] = perf_setup(0x81d0, 0, i);   // MEM_INST_RETIRED.ALL_LOADS
    perf_page[i][DRAMREAD] = perf_setup(0x1d3, 0, i, DRAMREAD);      // MEM_LOAD_L3_MISS_RETIRED.LOCAL_DRAM
    perf_page[i][NVMREAD] = perf_setup(0x80d1, 0, i, NVMREAD);     // MEM_LOAD_RETIRED.LOCAL_PMM
    //perf_page[i][WRITE] = perf_setup(0x82d0, 0, i, WRITE);    // MEM_INST_RETIRED.ALL_STORES
    //perf_page[i][WRITE] = perf_setup(0x12d0, 0, i);   // MEM_INST_RETIRED.STLB_MISS_STORES
  }

  pthread_mutex_init(&(dram_free_list.list_lock), NULL);
  for (int i = 0; i < dramsize / PAGE_SIZE; i++) {
    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    p->devdax_offset = i * PAGE_SIZE + dramoffset;
    p->present = false;
    p->in_dram = true;
    p->ring_present = false;
    p->pt = pagesize_to_pt(PAGE_SIZE);
    pthread_mutex_init(&(p->page_lock), NULL);

    enqueue_fifo(&dram_free_list, p);
  }

  pthread_mutex_init(&(nvm_free_list.list_lock), NULL);
  for (int i = 0; i < nvmsize / PAGE_SIZE; i++) {
    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    p->devdax_offset = i * PAGE_SIZE + nvmoffset;
    p->present = false;
    p->in_dram = false;
    p->ring_present = false;
    p->pt = pagesize_to_pt(PAGE_SIZE);
    pthread_mutex_init(&(p->page_lock), NULL);

    enqueue_fifo(&nvm_free_list, p);
  }

  pthread_mutex_init(&(dram_fifo_list.list_lock), NULL);
  pthread_mutex_init(&(nvm_fifo_list.list_lock), NULL);

  buffer = (uint64_t**)malloc(sizeof(uint64_t*) * CAPACITY);
  assert(buffer);
  //hot_ring = ring_buf_init(buffer, CAPACITY);
  buffer = (uint64_t**)malloc(sizeof(uint64_t*) * CAPACITY);
  assert(buffer);
  //cold_ring = ring_buf_init(buffer, CAPACITY);
  buffer = (uint64_t**)malloc(sizeof(uint64_t*) * CAPACITY);
  assert(buffer);
  free_page_ring = ring_buf_init(buffer, CAPACITY);

  LOG("Memory management policy is PEBS\n");

  LOG("pebs_init: finished\n");

}

void fifo_shutdown()
{
  for (int i = pebs_start_cpu; i < pebs_start_cpu + num_cores; i++) {
    for (int j = 0; j < NPBUFTYPES; j++) {
      ioctl(pfd[i][j], PERF_EVENT_IOC_DISABLE, 0);
      //munmap(perf_page[i][j], sysconf(_SC_PAGESIZE) * PERF_PAGES);
    }
  }
}

static inline double calc_miss_ratio()
{
  return ((1.0 * accesses_cnt[NVMREAD]) / (1.0 * (accesses_cnt[DRAMREAD] + accesses_cnt[NVMREAD])));
}

void fifo_stats()
{
  uint64_t total_samples = 0;
  LOG_STATS("\tdram_active_list.numentries: [%ld]\tdram_free_list.numentries: [%ld]\tnvm_active_list.numentries: [%ld]\tnvm_free_list.numentries: [%ld]\themem_pages: [%lu]\ttotal_pages: [%lu]\tzero_pages: [%ld]\tthrottle/unthrottle_cnt: [%ld/%ld]\tcools: [%ld]\n",
          dram_fifo_list.numentries,
          dram_free_list.numentries,
          nvm_fifo_list.numentries,
          nvm_free_list.numentries,
          hemem_pages_cnt,
          total_pages_cnt,
          zero_pages_cnt,
          throttle_cnt,
          unthrottle_cnt,
          cools);
  LOG_STATS("\tdram_accesses: [%lu]\tnvm_accesses: [%lu]\tsamples: [", accesses_cnt[DRAMREAD], accesses_cnt[NVMREAD]);
  for (int i = 0; i < PEBS_NPROCS ; i++) {
    LOG_STATS("%lu ", core_accesses_cnt[i]);
    total_samples += core_accesses_cnt[i];
    core_accesses_cnt[i] = 0;
  }
  LOG_STATS("]\ttotal_samples: [%lu]\n", total_samples);

  if (accesses_cnt[DRAMREAD] + accesses_cnt[NVMREAD] != 0) {
    if (miss_ratio == -1.0) {
      miss_ratio = calc_miss_ratio();
    } else {
      miss_ratio = (EWMA_FRAC * calc_miss_ratio()) + ((1 - EWMA_FRAC) * miss_ratio);
    }
  } else {
    miss_ratio = -1.0;
  }

  accesses_cnt[DRAMREAD] = accesses_cnt[NVMREAD] = 0;

  fprintf(stdout, "Total: %.2f GB DRAM, %.2f GB NVM\n",
    (double)(dram_fifo_list.numentries) * ((double)PAGE_SIZE) / (1024.0 * 1024.0 * 1024.0),
    (double)(nvm_fifo_list.numentries) * ((double)PAGE_SIZE) / (1024.0 * 1024.0 * 1024.0));
  fflush(stdout);
  hemem_pages_cnt = total_pages_cnt =  throttle_cnt = unthrottle_cnt = 0;
}