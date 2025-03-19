# HeMem

This document describes the artifact for our [SOSP 2021 paper](https://dl.acm.org/doi/10.1145/3477132.3483550 "SOSP 2021 paper") on HeMem. HeMem is a tiered main memory management system designed from scratch for commercially available NVM and the big data applications that use it. HeMem manages tiered memory asynchronously, batching and amortizing memory access tracking, migration, and associated TLB synchronization overheads. HeMem monitors application memory use by sampling memory access via CPU events, rather than page tables. This allows HeMem to scale to terabytes of memory, keeping small and ephemeral data structures in fast memory, and allocating scarce, asymmetric NVM bandwidth according to access patterns. Finally, HeMem is flexible by placing per-application memory management policy at user-level.

## Overview

* `apps/` contains the application benchmarks evaluated with HeMem
* `microbenchmarks/` contains the GUPS microbenchmark used to evaluate HeMem
* `src/` contains the source code of HeMem
	* `src/policies` contains extra memory policies used for testing HeMem, such as a page-table based LRU policy
* `Hoard/` contains the Hoard memory allocator that HeMem depends on
* `linux/` contains the linux kernel version required to run HeMem

### Building and Running HeMem

#### Setup

You may set up HeMem to run on your own machine provided you have Intel Optane NVM. HeMem uses `/dev/dax` files to represent DRAM and NVM. Some additional setup is required for setting up the DRAM and NVM `/dev/dax` files to run HeMem.

To set up the `/dev/dax` file representing DRAM, follow the instructions [here](https://pmem.io/2016/02/22/pm-emulation.html "here") in order to reserve a block of DRAM at machine startup to represent the DRAM `/dev/dax` file. HeMem reserves its 140GB of DRAM in this way (enough for its 128GB of reserved DRAM plus some metadata needed for `ndctl`). If your machine has multiple NUMA nodes, ensure that the block of DRAM you reserve is located on the same NUMA node that has NVM. **Do not follow the last set of instructions from pmem.io on setting up a file system on the reserved DRAM.** Instead, set up a `/dev/dax` file to represent it:

1. First, determine the name of the namespace representing the reserved DRAM:

`ndctl list --human`

2. You should see your reserved DRAM. If multiple namespaces are listed, some represent NVM namespaces (described below). You should be able to differentiate the DRAM namespace based on size. Your DRAM namespace is likely in `fsdax` mode. Change the namespace over to `devdax` mode using the following command (in this example, the DRAM namespace is called `namespace0.0`):

`sudo ndctl create-namespace -f -e namespace0.0 --mode=devdax --align 2M`

3. Make note of the `chardev` name of the DRAM `/dev/dax` file. This will be used to tell HeMem which `/dev/dax` file represents DRAM. If this is different from `dax0.0`, then you will need to set the environment variable `DRAMPATH` to your actual DRAM `/dev/dax` file.

To set up the `/dev/dax` file representing NVM, ensure that your machine has NVM in App Direct mode. If you do not already have namespaces representing NVM, then you will need to create them. Follow these steps:

1. List the regions available on your machine:

`ndctl list --regions --human`

2. Note which regions represent NVM. You can differentiate them from the reserved DRAM region based on size or via the `persistence_domain` field, which, for NVM, will read `memory_controller`. Pick the region that is on the same NUMA node as your reserved DRAM. In this example, this is "region1". Create a namespace over this region:

`ndctl create-namespace --region=1 --mode=devdax`

3. Make note of the `chardev` name of the NVM `/dev/dax` file. This will be used to tell HeMem which `/dev/dax` file represents NVM. If this is different from `dax1.0`, then you will need to set the environment variable `NVMPATH` to your actual DRAM `/dev/dax` file.


#### Building

To build HeMem, you must first build the linux kernel HeMem depends on. Build, install, and run the kernel located in the `linux/` directory.

Next, HeMem depends on Hoard. Follow the instructions to build the Hoard library located in the `Hoard/` directory.

HeMem also depends on libsyscall_intercept to intercept memory allocation system calls. Follow the instructions to build and install libsyscall_intercept [here](https://github.com/pmem/syscall_intercept).

Once the proper kernel version is running, the `/dev/dax` files have been set up, and all dependencies have been installed, HeMem can be built with the supplied Makefile by typing `make` from the `src/` directory.

#### Running

You will likely need to add the paths to the build HeMem library and the Hoard library to your LD_LIBRARY_PATH variable:

`export LD_LIBRARY_PATH=path/to/hemem/lib:/path/to/Hoard/lib:$LD_LIBRARY_PATH`

You may also need to increase the number of allowed mmap ranges:

`echo 1000000 > /proc/sys/vm/max_map_count`

HeMem requires the user be root in order to run. Applications can either be linked with Hemem or run unmodified via the `LD_PRELOAD` environment variable:

`LD_PRELOAD=/path/to/hemem/lib.so ./foo [args]`

### Microbenchmarks

A Makefile is provided to build the GUPS microbenchmarks.

To reproduce the Uniform GUPS results, run the `run-random.sh` script. Results will be printed to the `random.txt` file. The throughput results shown in the paper are the "GUPS" lines.

To reproduce the Hotset GUPS results, run the `run.sh` script. Results will be printed to the `results.txt` file. The throughput results shown in the paper are the "GUPS" lines.

To reproduce the Instantaneous GUPS results, run the `run-instantaneous.sh` script. Results will be printed to the `tot_gups.txt` file.

### Application Benchmarks

Applications tested with HeMem are located in the `apps/` directory.

#### Silo 

The Silo application can be found in the `apps/silo_hemem/silo` directory.. Run the provided `run_batch.sh` script. Results will be in the `batch/results.txt` file. The reported throughput numbers are numbers in the first column of the file.

#### FlexKVS

The FlexKVS application can be found in the `apps/flexkvs` directory. These results require a separate machine for the clients.

#### GapBS

The GapBS application can be found in the `apps/gapbs` directory. To run the BC algorithm reported in the paper, you may run the following command:

`LD_PRELOAD=/path/to/hemem/lib ./bc -g <scale>`

which will run the bc algorithm with HeMem on a graph with 2^scale vertices.

## Proactive HeMem

On top of the HeMem implementation, we have implemented Proactive HeMem, which is a version of HeMem that uses a proactive migration policy to move data between memory tiers. We implemented 3 different approaches to proactive migration. The link to the presentation slides for Proactive HeMem, which includes some initial results, can be found [here](https://docs.google.com/presentation/d/1EwmgLvLuy5wcBPVJ9qDFPuSjwzkhD60rcnaH2HqUroM/edit?usp=sharing).

### Stride-based Prefetching

Stride-based prefetching is a simple proactive migration policy that introduces **stride pattern detection** to optimize memory access performance. The system now tracks recent memory accesses per CPU core, identifying repeated stride-based access patterns. Upon detection, it **proactively prefetches** future memory pages and migrates them from NVM to DRAM in advance, reducing access latency.


This policy is implemented in the [pattern-prefetch](https://github.com/sach-12/proactive-hemem/tree/pattern-prefetch) branch of the HeMem repository. To build and run this version of HeMem, follow the same instructions as above, but use the `pattern-prefetch` branch of the HeMem repository. Key modifications include updates to `pebs_scan_thread` for analyzing memory patterns and a new `StridePattern` structure to track address strides per core. Configurable constants like `STRIDE_THRESHOLD = 2` and `PREFETCH_DISTANCE = 3` control detection sensitivity and prefetching behavior.

### Application Hints

Application hints is a proactive migration policy that introduces a **custom application-defined memory management approach**, allowing developers to define their own policies based on application-specific access patterns. The **FIFO-DRAM priority policy** keeps frequently accessed pages in DRAM and migrates older pages to NVM in FIFO order when needed.

This policy is implemented in the [app-hint](https://github.com/sach-12/proactive-hemem/tree/app-hint) branch of the HeMem repository. To build and run this version of HeMem, follow the same instructions as above, but use the `app-hint` branch of the HeMem repository. Key enhancements include the creation of a new **"fifo-dram" policy**, demonstrating the ease of custom policy development. This approach eliminates the need for separate **PEBS** or **policy threads**, reducing startup time. Designed for sequential access patterns, it optimizes memory access latency by keeping active pages in DRAM.

### Sequential Prefetching

Sequential prefetching is a proactive migration policy that enhances **memory access efficiency** by prefetching consecutive pages, optimizing sequential algorithms. When a hot page in the **NVM Hot List** is detected for migration, the system also **prefetches and migrates the next page** proactively. Experiments were conducted with different prefetch sizes: **1, 5, and 10 pages**.

This policy is implemented in the [pebs-prefetch](https://github.com/sach-12/proactive-hemem/tree/pebs-prefetch) branch of the HeMem repository. To build and run this version of HeMem, follow the same instructions as above, but use the `pebs-prefetch` branch of the HeMem repository. Key modifications include updates to the **PEBS policy thread** for migrating consecutive pages and enhancements to the **page struct** and **scan thread** to prevent immediate migration back from DRAM to NVM. These improvements reduce memory access latency by ensuring data is available **before demand**.
