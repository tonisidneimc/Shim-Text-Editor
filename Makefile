shim: shim.c
	$(CC) shim.c -o shim -Wall -Wextra -pedantic -std=c99

clean:
	@rm -f *.o shim *~ *.txt
