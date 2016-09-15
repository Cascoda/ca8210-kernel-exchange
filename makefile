TARGET = libca8210.a
LIBS = -lm
CFLAGS = -g -Wall -pthread
INCLUDEDIR = cascoda-api/include/
SOURCEDIR = ./
SUBDIRS = cascoda-api

.PHONY: default all clean subdirs $(SUBDIRS)

default: $(TARGET)
all: default

OBJECTS = $(patsubst %.c, %.o, $(wildcard $(SOURCEDIR)*.c))
HEADERS = $(wildcard $(INCLUDEDIR),*.h)

%.o: %.c $(HEADERS) subdirs
	$(CC) $(CFLAGS) -c $< -o $@ -I $(INCLUDEDIR)

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	cp cascoda-api/libcascoda.a ./libcascoda.a
	ar -x libcascoda.a
	ar rcs $(TARGET) *.o

clean:
	-rm -f $(SOURCEDIR)*.o
	-rm -f $(TARGET)

subdirs: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@