CXX = arm-linux-gnueabihf-g++
CC  = arm-linux-gnueabihf-gcc
CFLAGS = -Os -fPIC -fvisibility=hidden
CXXFLAGS = $(CFLAGS) -Wno-unused-result -Wno-unused-variable -Wno-unused-local-typedefs

all: libtolinom.so

libtolinom.so: tolinom.o nh.o
	$(CXX) -shared -o $@ $^ -ldl -lpthread
	arm-linux-gnueabihf-strip $@

tolinom.o: tolinom.cc NickelHook.h
	$(CXX) $(CXXFLAGS) -I. -c -o $@ $<

nh.o: nh.c NickelHook.h
	$(CC) $(CFLAGS) -I. -c -o $@ $<

clean:
	rm -f *.o libtolinom.so

.PHONY: all clean
