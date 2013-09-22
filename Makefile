LIBS = -ldl
CFLAGS =

CFLAGS += -DDEBUG

libenviable.so: enviable.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< $(LIBS)

clean:
	rm -f libenviable.so

.PHONY: clean
