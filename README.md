# Open CAS Framework - Guanzhou's Fork

This is Guanzhou's fork of Intel's Open-CAS Framework (OCF).


## Overview

Folder structure:

```text
# This is the user-level context
contexts/
 |- ul-exp/     # Context code wrapping OCF, for user-level benchmarking    
 |   |- src/
 |   |   |- cache/      # Cache volume FlashSim driver, queue, and log
 |   |   |- core/       # Core volume FlashSim driver, queue, and log
 |   |   |- simfs/      # Dummy application context
 |   |   |- fuzzy/      # Fuzzy testing workload (for correctness)
 |   |   |- bench/      # All benchmarking logics should go here
 |   |   |- main.c
 |   |   |- ...
 |   |- Makefile
 |   |- cache-ssd.conf  # Cache SSD configuration used in experiments
 |   |- core-ssd.conf   # Core SSD configuration used in experiments
 |   |- run-flashsim.sh
 |   |- ...
```

```text
# These are the OCF library - I added mf cache modes into the engine
src/
 |- env/
 |   |- posix/      # POSIX environment specific support
 |- inc/            # OCF headers exposed to context code
 |- src/            # OCF library source code
 |   |- engine/             
 |   |   |- engine_mfwa.c   # Multi-factor cache mode read with write-around
 |   |   |- engine_mfwa.h
 |   |   |- engine_mfwb.c   # Multi-factor cache mode read with write-back
 |   |   |- engine_mfwb.h
 |   |   |- mf_monitor.c    # Multi-factor monitor logic
 |   |   |- mf_monitor.h
 |   |   |- ...             # Some other files are also touched
 |   |- ...
```

Everything I have added into the OCF library are marked by `[Orthus FLAG BEGIN]` and `[Orthus FLAG END]` for easier future reference.


## Usage

Clone the repo recursively (there is a submodule - the Flash SSD simulator `flashsim`):

```bash
$ git clone --recursive git@github.com:josehu07/ocf-mf.git
$ cd ocf-mf
$ git submodule update --init --recursive
```

Go into the example context `ul-exp` and compile:

```bash
$ cd contexts/ul-exp
$ make
```

This will link the OCF library to this location and compile it together with your main file into a single executable `./bench`.

### Fuzzy Testing for Correctness

Ensure that the `PAGE_ENABLE_DATA` option in both cache and core FlashSim config files are set to `1`. Then, start cache and core FlashSim devices by:

```bash
# In shell 1:
$ ./run-flashsim.sh cache

# In shell 2:
$ ./run-flashsim.sh core
```

Then, in yet another shell:

```bash
# In shell 3:
$ ./bench <mode> fuzzy                      # E.g., ./bench mfwa fuzzy
Where:
    mode := pt|wa|wb|wt|mfwa|mfwb|mfwt
```

### Benchmarking Experiments

Ensure that the `PAGE_ENABLE_DATA` option in both cache and core FlashSim config files are set to `0`. Then, start cache and core FlashSim devices by:

```bash
# In shell 1:
$ ./run-flashsim.sh cache

# In shell 2:
$ ./run-flashsim.sh core
```

Then, in yet another shell:

```bash
# In shell 3:
$ ./bench <mode> <bench_name> [bench_args]  # E.g., ./bench mfwa throughput 12000 100 99
Where:
    mode := pt|wa|wb|wt|mfwa|mfwb|mfwt
    bench_name & bench_args are defined by benchmarks
```

The exact way of invoking `./bench` on experiments depends on the benchmarks code. The above example is for throughput experiments.

### Adding a New Benchmark

To add a new benchmarking experiment called `new_bench` for example, do:

1. Navigate to the folder `contexts/ul-exp/src/bench/`. There you will find benchmarking experiments implementation. Add `new_bench.c` & `new_bench.h` there. (Follow `throughput.c` & `throughput.h` as a guidance.)
2. In `contexts/ul-exp/src/main.c`, find the arrays `bench_names` and `bench_funcs` at the top. Add your new benchmark in. (Follow the `throughput` benchmark as a guidance.)

Your benchmark can take in multiple arguments. Just be sure to parse those arguments correctly in your benchmarking function.


## TODO List

- [x] Model in-device parallelism. For each volume, there is a submission queue (See `cache/cache-vol.c` and `core/core-vol.c`). Currently, it processes requests one at a time. However, it should process requests at a parallelism degree of the SSD's number of packages (channels).
- [x] Multi-factor cache mode with different write-allocation policies: Write-Around, Write-Back, and Write-Through
- [x] Refactor and modularize the benchmarking part.
- [ ] More benchmarking logics in `bench/...`.
- [ ] Porting the `mf` cache mode to Open CAS Linux: Need to ensure that it is implemented in a kernel-safe way.
- [ ] Better ways of measuring throughput? Currently, each backend keeps a log of finished requests (See `cache/cache-obj.c` and `core/core-obj.c`).


## Original README

[![Build Status](https://open-cas-logs.s3.us-east-2.amazonaws.com/master-status/build/curr-badge.svg)](https://open-cas-logs.s3.us-east-2.amazonaws.com/master-status/build/build.html)
[![Tests Status](https://open-cas-logs.s3.us-east-2.amazonaws.com/master-status/tests/curr-badge.svg)](https://open-cas-logs.s3.us-east-2.amazonaws.com/master-status/tests/index.html)
[![Coverity status](https://scan.coverity.com/projects/19083/badge.svg)](https://scan.coverity.com/projects/open-cas-ocf)
[![codecov](https://codecov.io/gh/Open-CAS/ocf/branch/master/graph/badge.svg)](https://codecov.io/gh/Open-CAS/ocf)
[![License](https://open-cas-logs.s3.us-east-2.amazonaws.com/master-status/license-badge.svg)](LICENSE)

Open CAS Framework (OCF) is high performance block storage caching meta-library
written in C. It's entirely platform and system independent, accessing system API
through user provided environment wrappers layer. OCF tightly integrates with the
rest of software stack, providing flawless, high performance, low latency caching
utility.

### In this readme:

* [Documentation](#documentation)
* [Source Code](#source-code)
* [Deployment](#deployment)
* [Examples](#examples)
* [Unit Tests](#unit-tests)
* [Build Test](#build-test)
* [Functional Tests](#functional-tests)
* [Contributing](#contributing)
* [Security](#security)

### Documentation

OCF documentation is available on [GitHub Pages](https://open-cas.github.io/getting_started_ocf.html).
Doxygen API documentation is available [here](http://open-cas.github.io/doxygen/ocf).

### Source Code

Source code is available in the official OCF GitHub repository:

~~~{.sh}
git clone https://github.com/Open-CAS/ocf.git
cd ocf
~~~

### Deployment

OCF doesn't compile as separate library. It's designed to be included into another
software stack. For this purpose OCF provides Makefile with two useful targets for
deploying its source into target directories. Assuming OCFDIR is OCF directory, and
SRCDIR and INCDIR are respectively your source and include directories, use following
commands to deploy OCF into your project:

~~~{.sh}
make -C $OCFDIR src O=$SRCDIR
make -C $OCFDIR inc O=$INCDIR
~~~

By default this will not copy OCF source files but create symbolic links to them,
to avoid source duplication and allow for easy OCF code modification. If you prefer
to copy OCF source files (e.g. you don't want to distribute whole OCF repository
as your submodule) you can use following commands:

~~~{.sh}
make -C $OCFDIR src O=$SRCDIR CMD=cp
make -C $OCFDIR inc O=$INCDIR CMD=cp
~~~

### Examples

OCF is shipped with examples, which are complete, compillable and working
programs, containing lot of comments that explain basics of caching. They
are great starting point for everyone who wants to start working with OCF.

Examples can be found in directory `example/`.

Each example contains Makefile which can be used to compile it.

### Unit Tests

OCF is shipped with dedicated unit test framework based on Cmocka.  
To run unit tests you need to install following packages:
- Cmake (>= 3.8.1)
- Cmocka (>= 1.1.1)
- ctags (>= 5.8)

To run unit tests use following command:

~~~{.sh}
./tests/unit/framework/run_unit_tests.py
~~~

### Build Test

OCF repository contains basic build test. It uses default POSIX environment.
To run this test, use following commands:

~~~{.sh}
cd tests/build/
make
~~~

### Functional Tests

OCF repository contains dedicated functional test framework written in python and executed via pytest. With the use of ctypes it is possible to call, wrap ocf functions and use C compatible data types.  
To run functional tests you need to install the following:
- python3 (>=3.6.7)
- pytest (Install with `pip3 install pytest`)  

To run all functional tests (in compliance with the configuration file) compile using makefile located in `./tests/functional/Makefile` and then use the following command:

~~~{.sh}
pytest
~~~

### Contributing

Feel like making OCF better? Don't hesitate to submit a pull request!  
You can find more information about our contribution process
[here](https://open-cas.github.io/contributing.html).  
In case of any questions feel free to contact [maintainer](mailto:robert.baldyga@intel.com).

### Security

To report a potential security vulnerability please follow the instructions
[here](https://open-cas.github.io/contributing.html#reporting-a-potential-security-vulnerability)
