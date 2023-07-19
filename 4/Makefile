GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant

all: test.o thread_pool.o thread_pool.o futex.o circular_queue.o
	gcc $(GCC_FLAGS) test.o thread_pool.o futex.o circular_queue.o

test.o: test.c
	gcc $(GCC_FLAGS) -c test.c -o test.o -I ../utils

thread_pool.o: thread_pool.c
	gcc $(GCC_FLAGS) -c thread_pool.c -o thread_pool.o

futex.o: futex.c
	gcc $(GCC_FLAGS) -c futex.c -o futex.o

circular_queue.o: circular_queue.c
	gcc $(GCC_FLAGS) -c circular_queue.c -o circular_queue.o
