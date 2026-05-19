# CompArch_Project
Project was created by Claude.ai.

## Step 1 — Check prerequisites
```bash
gcc --version
make --version
```

If either is missing:
```bash
# Ubuntu / Debian
sudo apt update && sudo apt install build-essential

# Fedora / RHEL
sudo dnf install gcc make
```

---

## Step 2 — Extract the archive

Download `uthreads.tar` from this chat, then:

```bash
tar -xzf uthreads.tar.gz
cd uthreads
```

---

## Step 3 — Build

```bash
make
```

You should see it compile 4 source files, archive them into `libuthreads.a`, then link 5 binaries. No errors expected — only one harmless warning about `_GNU_SOURCE` being redefined.

Expected output:
```
gcc ... -c src/thread.c  -o src/thread.o
gcc ... -c src/mutex.c   -o src/mutex.o
gcc ... -c src/sync.c    -o src/sync.o
gcc ... -c src/preempt.c -o src/preempt.o
ar rcs libuthreads.a src/thread.o src/mutex.o ...
gcc ... examples/philosophers.c     -luthreads -o examples/philosophers
gcc ... examples/readers_writers.c  -luthreads -o examples/readers_writers
gcc ... examples/producer_consumer.c -luthreads -o examples/producer_consumer
gcc ... examples/preempt_demo.c     -luthreads -o examples/preempt_demo
gcc ... bench/bench.c               -luthreads -o bench/bench_uth
gcc ... bench/bench.c -DUSE_PTHREAD -lpthread  -o bench/bench_pthread
```

---

## Step 4 — Run the examples

```bash
make test
```

This runs all four examples automatically. What you should see at the end of each:

```
Total meals eaten: 10  (expected 10)       ← philosophers
Final shared_data = 4  (expected 4)        ← readers-writers
Produced: 8  Consumed: 8                   ← producer-consumer
PASS — preemption works                    ← preempt_demo
```

---

## Step 5 — Run the benchmarks

```bash
make bench
```

This runs uthreads and pthreads back to back. You'll get numbers like:

```
=== uthreads ===
[uthreads] create+join 100 threads:   ~12,000 ns/thread
[uthreads] mutex contention:          ~800 Kops/s
[uthreads] context switches:          ~2,600 ns/switch

=== pthread ===
[pthread]  create+join 100 threads:  ~150,000 ns/thread   ← uthreads wins
[pthread]  mutex contention:         ~47,000 Kops/s        ← pthread wins
[pthread]  context switches:         ~1,000 ns/switch      ← pthread wins
```

---

## Step 6 — Clean and rebuild any time

```bash
make clean   # removes all .o files, .a, and binaries
make         # rebuild from scratch
```

---

## Full command sequence (copy-paste ready)

```bash
sudo apt update && sudo apt install build-essential   # if needed
tar -xzf uthreads.tar.gz
cd uthreads
make
make test
make bench
```


That's it. The whole thing from zero to passing tests takes under a minute on any modern Linux machine.