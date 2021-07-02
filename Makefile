shim: shim.c
	$(CC) shim.c -o shim -std=c99

debug: shim.c
	$(CC) shim.c -o shim -Wall -Wextra -pedantic -std=c99 -g

install: shim
	sudo cp shim /usr/local/bin
	sudo chmod +x /usr/local/bin

clean:
	@rm -f *.o shim *~ *.txt
