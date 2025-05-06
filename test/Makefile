CC := gcc
CFLAGS := -Wall -Wextra

SRCS := $(wildcard *.c)
OUTS := $(SRCS:.c=.out)

all: $(OUTS)

%.out: %.c
	@$(CC) $(CFLAGS) -o $@ $<

clean:
	@rm -f $(OUTS)
