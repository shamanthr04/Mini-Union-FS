CC = gcc
CFLAGS = -Wall -Wextra $(shell pkg-config --cflags fuse)
LDFLAGS = $(shell pkg-config --libs fuse)
TARGET = mini_unionfs
SRC = mini_unionfs.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

test: $(TARGET)
	bash test_unionfs.sh
