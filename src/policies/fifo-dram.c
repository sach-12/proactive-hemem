#define _GNU_SOURCE
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
#include "../pebs.h"
#include "../timer.h"
#include "../spsc-ring.h"

uint64_t pebs_start_cpu;
uint64_t migration_thread_cpu;
uint64_t scanning_thread_cpu;

static struct fifo_list dram_fifo_list;
static struct fifo_list nvm_fifo_list;
static struct fifo_list dram_free_list;
static struct fifo_list nvm_free_list;
static ring_handle_t hot_ring;
static ring_handle_t cold_ring;
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

void *pebs_scan_thread()
{
#ifdef SAMPLE_BASED_COOLING
  uint64_t samples_since_cool = 0;
#endif

  cpu_set_t cpuset;
  pthread_t thread;

  thread = pthread_self();
  CPU_ZERO(&cpuset);
  CPU_SET(scanning_thread_cpu, &cpuset);
  int s = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    perror("pthread_setaffinity_np");
    assert(0);
  }

  for(;;) {
    for (int i = pebs_start_cpu; i < pebs_start_cpu + num_cores; i++) {
      for(int j = 0; j < NPBUFTYPES; j++) {
        struct perf_event_mmap_page *p = perf_page[i][j];
        char *pbuf = (char *)p + p->data_offset;

        __sync_synchronize();

        if(p->data_head == p->data_tail) {
          continue;
        }

        struct perf_event_header *ph = (void *)(pbuf + (p->data_tail % p->data_size));
        struct perf_sample* ps;
        struct hemem_page* page;

        switch(ph->type) {
        case PERF_RECORD_SAMPLE:
            ps = (struct perf_sample*)ph;
            assert(ps != NULL);
            if(ps->addr != 0) {
              __u64 pfn = ps->addr & HUGE_PFN_MASK;
            
              page = get_hemem_page(pfn);
              if (page != NULL) {
                if (page->va != 0) {
                  page->accesses[j]++;
                  page->tot_accesses[j]++;
                  //if (page->accesses[WRITE] >= HOT_WRITE_THRESHOLD) {
                  //  if (!page->hot && !page->ring_present) {
                  //      make_hot_request(page);
                  //  }
                  //}
                  /*else*/ if (page->accesses[DRAMREAD] + page->accesses[NVMREAD] >= HOT_READ_THRESHOLD) {
                    if (!page->hot && !page->ring_present) {
                        make_hot_request(page);
                    }
                  }
                  else if (/*(page->accesses[WRITE] < HOT_WRITE_THRESHOLD) &&*/ (page->accesses[DRAMREAD] + page->accesses[NVMREAD] < HOT_READ_THRESHOLD)) {
                    if (page->hot && !page->ring_present) {
                        make_cold_request(page);
                    }
                 }

                  accesses_cnt[j]++;
                  core_accesses_cnt[i]++;

                  page->accesses[DRAMREAD] >>= (global_clock - page->local_clock);
                  page->accesses[NVMREAD] >>= (global_clock - page->local_clock);
                  //page->accesses[WRITE] >>= (global_clock - page->local_clock);
                  page->local_clock = global_clock;
                  #ifndef SAMPLE_BASED_COOLING
                  if (page->accesses[j] > PEBS_COOLING_THRESHOLD) {
                    global_clock++;
                    cools++;
                    need_cool_dram = true;
                    need_cool_nvm = true;
                  }
                  #else
                  if (samples_since_cool > SAMPLE_COOLING_THRESHOLD) {
                    global_clock++;
                    cools++;
                    need_cool_dram = true;
                    need_cool_nvm = true;
                    samples_since_cool = 0;
                  }
                  #endif
                }
                #ifdef SAMPLE_BASED_COOLING
                samples_since_cool++;
                #endif
                hemem_pages_cnt++;
              }
              else {
                other_pages_cnt++;
              }
            
              total_pages_cnt++;
            }
            else {
              zero_pages_cnt++;
            }
  	      break;
        case PERF_RECORD_THROTTLE:
        case PERF_RECORD_UNTHROTTLE:
          //fprintf(stderr, "%s event!\n",
          //   ph->type == PERF_RECORD_THROTTLE ? "THROTTLE" : "UNTHROTTLE");
          if (ph->type == PERF_RECORD_THROTTLE) {
              throttle_cnt++;
          }
          else {
              unthrottle_cnt++;
          }
          break;
        default:
          fprintf(stderr, "Unknown type %u\n", ph->type);
          //assert(!"NYI");
          break;
        }

        p->data_tail += ph->size;
      }
    }
  }

  return NULL;
}

static void pebs_migrate_down(struct hemem_page *page, uint64_t offset)
{
  struct timeval start, end;

  gettimeofday(&start, NULL);

  page->migrating = true;
  hemem_wp_page(page, true);
  hemem_migrate_down(page, offset);
  page->migrating = false; 

  gettimeofday(&end, NULL);
  LOG_TIME("migrate_down: %f s\n", elapsed(&start, &end));
}

static void pebs_migrate_up(struct hemem_page *page, uint64_t offset)
{
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

#ifdef COOL_IN_PLACE
struct hemem_page* partial_cool(struct fifo_list *hot, struct fifo_list *cold, bool dram, struct hemem_page* current)
{
  struct hemem_page *p;
  uint64_t tmp_accesses[NPBUFTYPES];

  if (dram && !need_cool_dram) {
      return current;
  }
  if (!dram && !need_cool_nvm) {
      return current;
  }

  if (start_dram_page == NULL && dram) {
      start_dram_page = hot->last;
  }

  if (start_nvm_page == NULL && !dram) {
      start_nvm_page = hot->last;
  }

  for (int i = 0; i < COOLING_PAGES; i++) {
    next_page(hot, current, &p);
    if (p == NULL) {
        break;
    }
    if (dram) {
        assert(p->in_dram);
    }
    else {
        assert(!p->in_dram);
    }

    for (int j = 0; j < NPBUFTYPES; j++) {
        tmp_accesses[j] = p->accesses[j] >> (global_clock - p->local_clock);
    }

    if (/*(tmp_accesses[WRITE] < HOT_WRITE_THRESHOLD) &&*/ (tmp_accesses[DRAMREAD] + tmp_accesses[NVMREAD] < HOT_READ_THRESHOLD)) {
        p->hot = false;
    }
    
    if (dram && (p == start_dram_page)) {
        start_dram_page = NULL;
        need_cool_dram = false;
    }

    if (!dram && (p == start_nvm_page)) {
        start_nvm_page = NULL;
        need_cool_nvm = false;
    } 

    if (!p->hot) {
        current = p->next;
        page_list_remove_page(hot, p);
        enqueue_fifo(cold, p);
    }
    else {
        current = p;
    }
  }

  return current;
}
#else
static void partial_cool(struct fifo_list *hot, struct fifo_list *cold, bool dram)
{
  struct hemem_page *p;
  uint64_t tmp_accesses[NPBUFTYPES];

  if (dram && !need_cool_dram) {
      return;
  }
  if (!dram && !need_cool_nvm) {
      return;
  }

  if ((start_dram_page == NULL) && dram) {
      start_dram_page = hot->last;
  }

  if ((start_nvm_page == NULL) && !dram) {
      start_nvm_page = hot->last;
  }

  for (int i = 0; i < COOLING_PAGES; i++) {
    p = dequeue_fifo(hot);
    if (p == NULL) {
        break;
    }
    if (dram) {
        assert(p->in_dram);
    }
    else {
        assert(!p->in_dram);
    }

    for (int j = 0; j < NPBUFTYPES; j++) {
        tmp_accesses[j] = p->accesses[j] >> (global_clock - p->local_clock);
    }

    if (/*(tmp_accesses[WRITE] < HOT_WRITE_THRESHOLD) &&*/ (tmp_accesses[DRAMREAD] + tmp_accesses[NVMREAD] < HOT_READ_THRESHOLD)) {
        p->hot = false;
    }

    if (dram && (p == start_dram_page)) {
        start_dram_page = NULL;
        need_cool_dram = false;
    }

    if (!dram && (p == start_nvm_page)) {
        start_nvm_page = NULL;
        need_cool_nvm = false;
    } 

    if (p->hot) {
      enqueue_fifo(hot, p);
    }
    else {
      enqueue_fifo(cold, p);
    }
  }
}
#endif

void *pebs_policy_thread() {
    struct timeval start, end;
    struct hemem_page *p, *np;
    uint64_t old_offset;
    double migrate_time;

    for (;;) {
        gettimeofday(&start, NULL);

        p = dequeue_fifo(&dram_fifo_list);
        if (p != NULL) {
            np = dequeue_fifo(&nvm_free_list);
            if (np != NULL) {
                assert(!(np->present));

                old_offset = p->devdax_offset;
                pebs_migrate_down(p, np->devdax_offset);

                np->devdax_offset = old_offset;
                np->in_dram = true;
                np->present = false;

                enqueue_fifo(&nvm_fifo_list, p);
                enqueue_fifo(&dram_free_list, np);
            }
        }

        gettimeofday(&end, NULL);
        migrate_time = elapsed(&start, &end) * 1000000.0;
        if (migrate_time < (1.0 * PEBS_KSWAPD_INTERVAL)) {
            usleep((uint64_t)((1.0 * PEBS_KSWAPD_INTERVAL) - migrate_time));
        }

        LOG_TIME("migrate: %f s\n", elapsed(&start, &end));
    }

    return NULL;
}


static struct hemem_page* pebs_allocate_page() {
    struct timeval start, end;
    struct hemem_page *page, *evict_page, *new_nvm_page;

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

            pebs_migrate_down(evict_page, new_nvm_page->devdax_offset);
            new_nvm_page->devdax_offset = evict_page->devdax_offset;
            new_nvm_page->in_dram = false;
            new_nvm_page->present = true;

            enqueue_fifo(&nvm_fifo_list, evict_page);
            enqueue_fifo(&dram_free_list, new_nvm_page);
        } else {
            enqueue_fifo(&dram_fifo_list, evict_page);
            assert(!"Out of memory in NVM!");
        }
    }

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

    assert(!"Out of memory!");
}


struct hemem_page* fifo_pagefault(void)
{
  struct hemem_page *page;

  // do the heavy lifting of finding the devdax file offset to place the page
  page = pebs_allocate_page();
  assert(page != NULL);

  return page;
}

void fifo_remove_page(struct hemem_page *page)
{
  assert(page != NULL);

  LOG("pebs: remove page, put this page into free_page_ring: va: 0x%lx\n", page->va);

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

  LOG("pebs_init: started\n");

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
  
  scanning_thread_cpu = hemem_start_cpu;
  migration_thread_cpu = scanning_thread_cpu + 1;

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
  hot_ring = ring_buf_init(buffer, CAPACITY);
  buffer = (uint64_t**)malloc(sizeof(uint64_t*) * CAPACITY);
  assert(buffer); 
  cold_ring = ring_buf_init(buffer, CAPACITY);
  buffer = (uint64_t**)malloc(sizeof(uint64_t*) * CAPACITY);
  assert(buffer); 
  free_page_ring = ring_buf_init(buffer, CAPACITY);

  int r = pthread_create(&scan_thread, NULL, pebs_scan_thread, NULL);
  assert(r == 0);
  
  r = pthread_create(&kswapd_thread, NULL, pebs_policy_thread, NULL);
  assert(r == 0);
  
  LOG("Memory management policy is PEBS\n");

  LOG("pebs_init: finished\n");

}

void pebs_shutdown()
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
  LOG_STATS("\tfastmem_allocated: \tslowmem_allocated: \n");
}
