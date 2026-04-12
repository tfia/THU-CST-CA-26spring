.PHONY: all clean

all: matmul probe

matmul: matmul.cpp
	g++ -O0 -o build/matmul matmul.cpp

probe: probe.cpp
	g++ -O2 -march=native -o build/probe probe.cpp

clean:
	rm -f build/matmul build/probe
