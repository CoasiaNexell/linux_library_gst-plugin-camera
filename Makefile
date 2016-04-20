CFLAGS = -Wall -fPIC
INCLUDES := -I../sysroot/include \
	    -I./ \
	    -I../buildroot/output/staging/usr/include \
	    -I../buildroot/output/staging/usr/lib/glib-2.0/include \
	    -I../buildroot/output/staging/usr/include/glib-2.0 \
	    -I../buildroot/output/staging/usr/include/gstreamer-1.0
LDFLAGS := -L../sysroot/lib -L../buildroot/output/target/usr/lib
LIBS := -lnx-drm-allocator -lnx-v4l2 -lgstapp-1.0 -lgstallocators-1.0 -lgstbase-1.0 -lgmodule-2.0 -lgobject-2.0 -lglib-2.0

CROSS_COMPILE := aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc

SRCS := $(wildcard *.c)
OBJS := $(SRCS:.c=.o)

NAME := gstnxcamerasrc
LIB_TARGET := lib$(NAME).so
APP_TARGET := test_$(NAME)

.c.o:
	$(CC) $(INCLUDES) $(CFLAGS) -c $^

$(LIB_TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -shared -Wl,-soname,$(LIB_TARGET) -o $@ $^ $(LIBS)

all: $(LIB_TARGET)

install: $(LIB_TARGET)
	cp $^ ../sysroot/lib

.PHONY: clean

clean:
	rm -f *.o
	rm -f $(LIB_TARGET)
	rm -f $(APP_TARGET)
