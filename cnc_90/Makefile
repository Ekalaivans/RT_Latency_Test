CC := gcc
CFLAGS := -O3 -Wall -Wextra -pthread
LDFLAGS := -pthread -lrt

TARGET := rt_cnc_90
SRC := rt_cnc_isolation.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o *.txt *.csv *.png

.PHONY: all clean
