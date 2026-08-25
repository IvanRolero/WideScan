.PHONY: default clean

CFLAGS+= -Wall -Wextra -O2 -Wno-clobbered -fpermissive -Wno-missing-field-initializers -static

LDFLAGS+= ctx_scan_2000.dll -ljpeg -lgdi32 -luser32 -lcomdlg32 -lshell32 -lole32 -mwindows 

CC=g++

WINDRES=windres

SCAN_OS=widescan.cpp StdAfx.cpp
SCAN_HS=ScannerAttributes.h SetWindowParams.h StdAfx.h utils.h resource.h

RES_OBJ=app.res

default: widescan.exe

$(RES_OBJ): app.rc resource.h
	$(WINDRES) app.rc -O coff -o $(RES_OBJ)

widescan.exe: $(SCAN_OS) $(SCAN_HS) $(RES_OBJ)
	$(CC) -o $@ $(SCAN_OS) $(RES_OBJ) $(CFLAGS) $(LDFLAGS)

clean:
	rm -f widescan.exe $(RES_OBJ)