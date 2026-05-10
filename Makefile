# Makefile for uthreads library + examples + benchmarks
#
# Targets
# ───────
#   make              → build library + all examples + benchmarks
#   make test         → run all examples with small parameters
#   make bench        → run both uthreads and pthread benchmarks
#   make clean        → remove all build artifacts

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 \
          -I include \
          -D_GNU_SOURCE \
          -g
LDFLAGS =

# Library source files
LIB_SRCS = src/thread.c src/mutex.c src/sync.c src/preempt.c
LIB_OBJS = $(LIB_SRCS:.c=.o)
LIB      = libuthreads.a

# ------------------------------------------------------------------ #
# Default target                                                      #
# ------------------------------------------------------------------ #

.PHONY: all
all: $(LIB) \
     examples/philosophers \
     examples/readers_writers \
     examples/producer_consumer \
     examples/preempt_demo \
     bench/bench_uth \
     bench/bench_pthread

# ------------------------------------------------------------------ #
# Library                                                             #
# ------------------------------------------------------------------ #

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# ------------------------------------------------------------------ #
# Examples                                                            #
# ------------------------------------------------------------------ #

examples/philosophers: examples/philosophers.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -luthreads $(LDFLAGS) -o $@

examples/readers_writers: examples/readers_writers.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -luthreads $(LDFLAGS) -o $@

examples/producer_consumer: examples/producer_consumer.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -luthreads $(LDFLAGS) -o $@

examples/preempt_demo: examples/preempt_demo.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -luthreads $(LDFLAGS) -o $@

# ------------------------------------------------------------------ #
# Benchmarks                                                          #
# ------------------------------------------------------------------ #

bench/bench_uth: bench/bench.c $(LIB)
	$(CC) $(CFLAGS) $< -L. -luthreads $(LDFLAGS) -o $@

bench/bench_pthread: bench/bench.c
	$(CC) $(CFLAGS) -DUSE_PTHREAD $< -lpthread $(LDFLAGS) -o $@

# ------------------------------------------------------------------ #
# Test targets                                                        #
# ------------------------------------------------------------------ #

.PHONY: test
test: all
	@echo ""
	@echo "========================================"
	@echo " Dining Philosophers (5 philosophers, 2 meals each)"
	@echo "========================================"
	./examples/philosophers 5 2

	@echo ""
	@echo "========================================"
	@echo " Readers-Writers (3 readers, 2 writers, 2 iterations)"
	@echo "========================================"
	./examples/readers_writers 3 2 2

	@echo ""
	@echo "========================================"
	@echo " Producer-Consumer (2 producers, 3 consumers, 4 items each)"
	@echo "========================================"
	./examples/producer_consumer 2 3 4

	@echo ""
	@echo "========================================"
	@echo " Preemption demo (no yield calls; SIGALRM forces switching)"
	@echo "========================================"
	./examples/preempt_demo

.PHONY: bench
bench: bench/bench_uth bench/bench_pthread
	@echo ""
	@echo "========================================"
	@echo " uthreads benchmark"
	@echo "========================================"
	./bench/bench_uth

	@echo ""
	@echo "========================================"
	@echo " pthread benchmark"
	@echo "========================================"
	./bench/bench_pthread

# ------------------------------------------------------------------ #
# Clean                                                               #
# ------------------------------------------------------------------ #

.PHONY: clean
clean:
	rm -f $(LIB_OBJS) $(LIB)
	rm -f examples/philosophers
	rm -f examples/readers_writers
	rm -f examples/producer_consumer
	rm -f examples/preempt_demo
	rm -f bench/bench_uth bench/bench_pthread
