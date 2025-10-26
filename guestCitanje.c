#include <stdint.h>

#define FILE_PORT  0x0278
#define FILE_OPEN  1
#define FILE_CLOSE 2
#define FILE_READ  3
#define FILE_WRITE 4


#define SERIAL_PORT 0xE9

static inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0,%1" : : "a"(value), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t value) {
    asm volatile("outl %0,%1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1,%0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile("inl %1,%0" : "=a"(ret) : "Nd"(port));
    return ret;
}


static void out_string(uint16_t port, const char *s) {
    while (*s)
        outb(port, *s++);
}

static void serial_puts(const char *s) {
    while (*s)
        outb(SERIAL_PORT, *s++);
}






static uint8_t file_open(const char *path) {
    outb(FILE_PORT, FILE_OPEN);
    out_string(FILE_PORT, path);
    outb(FILE_PORT, 0);  
    return inb(FILE_PORT);  
}


static void file_close(uint8_t fd) {
    outb(FILE_PORT, FILE_CLOSE);
    outl(FILE_PORT, fd);
}


static uint8_t file_read(uint8_t fd) {
    outb(FILE_PORT, FILE_READ);
    outl(FILE_PORT, fd);
    return inb(FILE_PORT);  
}


static void file_write(uint8_t fd, const char *data, uint32_t len) {
    outb(FILE_PORT, FILE_WRITE);
    outl(FILE_PORT, fd);
    outl(FILE_PORT, len);
    out_string(FILE_PORT, data);
}




void __attribute__((noreturn))
__attribute__((section(".start")))
_start(void)
{
    serial_puts("Guest started\n");

    
    uint8_t fd = file_open("output.txt");
    
  

    /* for(int i = 0; i < 15; i++) {
        uint8_t read_byte = file_read(fd);
        outb(SERIAL_PORT, read_byte);
    } */

    while(1){
      

        uint8_t read_byte = file_read(fd);
        if(read_byte == 0) break;
        outb(SERIAL_PORT, read_byte);
    }

    
    file_close(fd);

    serial_puts("Guest done\n");

    for (;;)
        asm volatile("hlt");
}
