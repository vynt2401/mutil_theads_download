# Makefile for Linux C++ Downloader Service
CC = g++
CFLAGS = -Wall -g
LIBS = -lpthread

TARGET = downloader-v4
SOURCES = downloader-v4.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES) $(LIBS)

clean:
	rm -f $(TARGET) requests.txt
	rm -rf done/

.PHONY: all clean
