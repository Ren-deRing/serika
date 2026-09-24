#include <serika/compiler.h>

#include <asm/io.h>

#include <stdbool.h>

#define SERIAL_UART 0x3F8

bool initialized = false;

void serial_init() {
    outb(SERIAL_UART + 1, 0x00); // no interrupt mode (polled mode)
    outb(SERIAL_UART + 3, 0x80); // DLAB on (speed setting mode)
    outb(SERIAL_UART + 0, 0x01); // baud rate 115200 (Low)
    outb(SERIAL_UART + 1, 0x00); // baud rate 115200 (High)
    outb(SERIAL_UART + 3, 0x03); // DLAB Off + 8 bits of text, no parity, 1 stop bit
    outb(SERIAL_UART + 2, 0xC7); // FIFO enable, clear FIFO, 14-byte threshold
    outb(SERIAL_UART + 4, 0x0B); // DTR, RTS, OUT2 set

    initialized = true;
}

notrace void serial_putc(char c) {
    if (!initialized) return;
    while ((inb(SERIAL_UART + 5) & 0x20) == 0);
    outb(0x3F8, c);
}
