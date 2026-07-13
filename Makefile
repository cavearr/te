te:
	gcc -o te te.c -lcurses -DMAIN -DCURSES

clean:
	rm -f te

.PHONY: clean
