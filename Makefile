EXECUTABLE := pa-volume-watcher
SRCS := pa_volume_watcher.c
OBJS := $(SRCS:.c=.o)

CFLAGS := $(shell pkg-config --cflags libpulse) -g
LFLAGS := $(shell pkg-config --libs libpulse)

.PHONY: all clean

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJS)
	gcc -o $@ $^ $(LFLAGS)


%.o: %.c
	gcc -c $(CFLAGS) $<

clean:
	rm -f *.o
	rm -f $(EXECUTABLE)