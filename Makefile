all:
	gcc pty.c cJSON.c -o rty -g -ldl -lm -lpthread
clean:
	rm rty
