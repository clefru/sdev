all: sdev

sdev:	sdev.o mt19937ar.o
	cc -o $@ $^
