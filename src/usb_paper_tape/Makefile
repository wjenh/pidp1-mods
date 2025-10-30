CC=gcc
CFLAGS=-Wall -Wextra -std=c99 -O2
TARGET3=pdp1_usb_monitor
SOURCE3=pdp1_usb_monitor.c
LIBS=-ludev

all: $(TARGET) $(TARGET2) $(TARGET3)

$(TARGET3): $(SOURCE3)
	$(CC) $(CFLAGS) -o $(TARGET3) $(SOURCE3)

clean:
	rm -f $(TARGET) $(TARGET2) $(TARGET3)

#install: $(TARGET3)
#	sudo cp $(TARGET3) /usr/local/bin/
#	sudo chmod +x /usr/local/bin/$(TARGET3)

#.PHONY: all clean install
