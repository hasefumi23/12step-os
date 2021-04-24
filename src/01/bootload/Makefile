PREFIX  = /usr/local
ARCH    = h8300-elf
BINDIR  = $(PREFIX)/bin
ADDNAME = $(ARCH)-

AR = $(BINDIR)/$(ADDNAME)ar
AS = $(BINDIR)/$(ADDNAME)as
CC = $(BINDIR)/$(ADDNAME)gcc
LD = $(BINDIR)/$(ADDNAME)ld
NM = $(BINDIR)/$(ADDNAME)nm
OBJCOPY = $(BINDIR)/$(ADDNAME)objcopy
OBJDUMP = $(BINDIR)/$(ADDNAME)objdump
RANLIB = $(BINDIR)/$(ADDNAME)ranlib
STRIP = $(BINDIR)/$(ADDNAME)strip

#H8WRITE = ../../tools/h8write/kz_h8write
#H8WRITE = ..\..\..\PackageFiles\bin\kz_h8write.exe
H8WRITE = ../../../PackageFiles/bin/kz_h8write.exe

# FreeBSD-4.x:/dev/cuaaX, FreeBSD-6.x:/dev/cuadX, FreeBSD(USB):/dev/cuaUx
# Linux:/dev/ttySx, Linux(USB):/dev/ttyUSBx, Windows:comX
# H8WRITE_SERDEV = /dev/ttyS3 # シリアル接続
H8WRITE_SERDEV = COM4 # シリアル接続

OBJS = vector.o startup.o intr.o main.o interrupt.o
OBJS += lib.o serial.o xmodem.o elf.o

# 生成する実行形式のファイル名
TARGET = kzload

CFLAGS = -Wall -mh -nostdinc -nostdlib -fno-builtin
#CFLAGS += -mint32 # intを32ビットにすると掛算/割算ができなくなる
CFLAGS += -I.
#CFLAGS += -g
CFLAGS += -Os
CFLAGS += -DKZLOAD

LFLAGS = -static -T ld.scr -L.

.SUFFIXES: .c .o
.SUFFIXES: .s .o
.SUFFIXES: .S .o

all : $(TARGET)

# 実行形式の生成ルール
$(TARGET) : $(OBJS)
						$(CC) $(OBJS) -o $(TARGET) $(CFLAGS) $(LFLAGS)
						cp $(TARGET) $(TARGET).elf
						$(STRIP) $(TARGET)

# *.cファイルのコンパイルルール
.c.o : $<
			$(CC) -c $(CFLAGS) $<

# アセンブラファイルのアセンブルルール
.s.o : $<
			$(CC) -c $(CFLAGS) $<

.S.o : $<
			$(CC) -c $(CFLAGS) $<

# モトローラSレコードフォーマットへの変換ルール
$(TARGET).mot : $(TARGET)
								$(OBJCOPY) -O srec $(TARGET) $(TARGET).mot

image : $(TARGET).mot

# フラッシュROMへの転送
write : $(TARGET).mot
				$(H8WRITE) -3069 -f20 $(TARGET).mot $(H8WRITE_SERDEV)

# フォルダの掃除
clean :
				rm -f $(OBJS) $(TARGET).elf $(TARGET).mot
