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

local/lib/libogg.so: thirdparty/build.sh
	cd thirdparty && ./build.sh && cd ..

local/lib/libtheora.so: thirdparty/build.sh
	cd thirdparty && ./build.sh && cd ..

local/lib/libvorbis.so: thirdparty/build.sh
	cd thirdparty && ./build.sh && cd ..

local/lib/libsydneyaudio.a: thirdparty/build.sh
	cd thirdparty && ./build.sh && cd ..

plogg.o: plogg.cpp local/lib/libogg.so local/lib/libtheora.so local/lib/libvorbis.so local/lib/libsydneyaudio.a
	g++ -O3 -g -c $(INCLUDE) -I../leonora/dist/include -Ilocal/include -I../gst-plugin-bc/module -o plogg.o plogg.cpp

plogg: plogg.o 
	g++ -O3 -g -o plogg plogg.o local/lib/libsydneyaudio.a -L../leonora/dist/lib -Llocal/lib -lvorbis -ltheoradec -logg -lSDL -lX11 -lEGL -lGLESv2 $(LIBS) cmem.o470MV

clean: 
	rm *.o plogg
