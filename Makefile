CXX      := g++
TARGET   := rt_cnc_isolation non_isolate
SRC      := rt_cnc_isolation.c non_isolate.c

CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -g
LDFLAGS  := -pthread

.PHONY: all clean setcap run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -x c++ -o $@ $< $(LDFLAGS)

setcap: $(TARGET)
	sudo setcap cap_sys_nice,cap_ipc_lock=eip $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
