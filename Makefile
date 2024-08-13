all:
	gcc pty.c cJSON.c -o rty -g -ldl -lm
clean:
	rm rty
