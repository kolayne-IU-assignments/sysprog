GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant

all: lib exe test

lib: partial_message_queue.c chat.c chat_client.c chat_server.c
	gcc $(GCC_FLAGS) -c partial_message_queue.c -o partial_message_queue.o
	gcc $(GCC_FLAGS) -c chat.c -o chat.o
	gcc $(GCC_FLAGS) -c chat_client.c -o chat_client.o
	gcc $(GCC_FLAGS) -c chat_server.c -o chat_server.o

exe: lib chat_client_exe.c chat_server_exe.c
	gcc $(GCC_FLAGS) chat_client_exe.c chat.o chat_client.o \
		partial_message_queue.o -o client
	gcc $(GCC_FLAGS) chat_server_exe.c chat.o chat_server.o \
		partial_message_queue.o -o server

build_test: lib
	gcc $(GCC_FLAGS) test.c chat.o chat_client.o chat_server.o  \
		partial_message_queue.o -o test \
		-I ../utils -lpthread

test: build_test
	./test

clean:
	rm *.o
	rm client server test
