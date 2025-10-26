# ===== Makefile for KVM mini hypervisor project =====
# Builds both:
#   - mini_hypervisor (the hypervisor)
#   - guest.img (the guest binary image)
#
# Usage:
#   make            -> build everything
#   make clean      -> remove build artifacts
# =====================================================

CC = gcc
LD = ld

CFLAGS = -m64 -O2 -Wall -Wextra -pthread
GUEST_CFLAGS = -m64 -ffreestanding -fno-pic -nostdlib -nostartfiles -O2

all: guest.img kvm_zadatak3

kvm_zadatak3: kvm_zadatak3.c
	$(CC) $(CFLAGS) kvm_zadatak3.c -o kvm_zadatak3

# ===== Guest build =====
guest.img: guest.o guest.ld
	$(LD) -T guest.ld guest.o -o guest.img

guest.o: guest.c
	$(CC) $(GUEST_CFLAGS) -c -o guest.o guest.c

run: 
	$(CC) $(GUEST_CFLAGS) -c -o guest.o guest.c
	$(LD) -T guest.ld guest.o -o guest.img
	$(CC) $(CFLAGS) kvm_zadatak3.c -o kvm_zadatak3
	./kvm_zadatak3 --memory 4 --page 2 --guest guest.img   --file output.txt

testa: 
	$(CC) $(GUEST_CFLAGS) -c -o guestReadOnly.o guestReadOnly.c
	$(LD) -T guest.ld guestReadOnly.o -o guestReadOnly.img
	$(CC) $(CFLAGS) kvm_zadatak3.c -o kvm_zadatak3 			
	./kvm_zadatak3 --memory 4 --page 2 --guest guestReadOnly.img   --file output.txt
testb: 
	$(CC) $(GUEST_CFLAGS) -c -o guestPisanje.o guestPisanje.c
	$(LD) -T guest.ld guestPisanje.o -o guestPisanje.img
	$(CC) $(CFLAGS) kvm_zadatak3.c -o kvm_zadatak3
	./kvm_zadatak3 --memory 4 --page 2 --guest guestPisanje.img   --file output.txt
testc: 
	$(CC) $(GUEST_CFLAGS) -c -o guestPisanje.o guestPisanje.c
	$(LD) -T guest.ld guestPisanje.o -o guestPisanje.img
	$(CC) $(CFLAGS) kvm_zadatak3.c -o kvm_zadatak3 			
	./kvm_zadatak3 --memory 4 --page 2 --guest guestPisanje.img guestPisanje.img guestPisanje.img guestPisanje.img --file output.txt

	
# ===== Clean ===== make testc
clean:
	rm -f mini_hypervisor *.o *.img

.PHONY: all clean

