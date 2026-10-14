CC      ?= cc
CFLAGS  ?= -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Wpedantic
LDFLAGS += -pthread
SRC     := src/server.c src/hashtable.c src/queue.c

.PHONY: all test bench asan tsan clean

all: build/kvserver build/bench

build:
	mkdir -p build

build/kvserver: $(SRC) src/*.h | build
	$(CC) $(CFLAGS) -pthread -o $@ $(SRC) $(LDFLAGS)

build/bench: bench/bench.c | build
	$(CC) $(CFLAGS) -pthread -o $@ $< $(LDFLAGS)

build/test_hashtable: tests/test_hashtable.c src/hashtable.c | build
	$(CC) $(CFLAGS) -pthread -o $@ $^ $(LDFLAGS)

test: build/kvserver build/test_hashtable
	./build/test_hashtable
	python3 tests/test_server.py

bench: all
	./build/kvserver -p 7070 -t 8 & PID=$$!; sleep 0.5; \
	./build/bench 127.0.0.1 7070 8 20000; STATUS=$$?; \
	kill -INT $$PID; wait $$PID; exit $$STATUS

# Memory-error and data-race checks
asan: clean
	$(MAKE) test CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-pthread -fsanitize=address,undefined"

tsan: clean
	$(MAKE) test CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -fsanitize=thread" LDFLAGS="-pthread -fsanitize=thread"

clean:
	rm -rf build
