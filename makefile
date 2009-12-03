UNAME=$(shell uname -s)
ifeq "$(UNAME)" "Linux"
INCLUDE=
LIBS=-lasound
endif

ifeq "$(UNAME)" "Darwin"
INCLUDE=-I/opt/local/include
LIBS=-framework Carbon -framework CoreAudio -framework AudioToolbox -framework AudioUnit -framework Cocoa
endif

all: plogg

local/lib/libogg.a: thirdparty/build.sh
	cd thirdparty && ./build.sh && cd ..

local/lib/libtheora.a: thirdparty/build.sh
	cd thirdparty && ./build.sh && cd ..

local/lib/libvorbis.a: thirdparty/build.sh
	cd thirdparty && ./build.sh && cd ..

local/lib/libsydneyaudio.a: thirdparty/build.sh
	cd thirdparty && ./build.sh && cd ..

plogg.o: plogg.cpp local/lib/libogg.a local/lib/libtheora.a local/lib/libvorbis.a local/lib/libsydneyaudio.a
	g++ -O3 -g -c $(INCLUDE) -Ilocal/include -I../gst-plugin-bc/module -o plogg.o plogg.cpp

plogg: plogg.o 
	g++ -O3 -g -o plogg plogg.o local/lib/libsydneyaudio.a local/lib/libvorbis.a local/lib/libtheora.a local/lib/libogg.a -lSDL -lX11 -lEGL -lGLESv2 $(LIBS)

clean: 
	rm *.o plogg
