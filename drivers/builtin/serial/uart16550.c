#include <serika/compiler.h>
#include <serika/intc.h>
#include <serika/irq.h>
#include <serika/panic.h>
#include <serika/serial.h>
#include <serika/tty.h>

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

static long uart_write(struct tty_struct *tty, const char *buf, size_t n) {
    (void)tty;

    for (size_t i = 0; i < n; i++) serial_putc(buf[i]);
    return n;
}

static const struct tty_operations uart_ops = {
    .write = uart_write,
};

static void uart_irq(struct trapframe *tf, void *data) {
    (void)tf;

    struct tty_struct *tty = data;

    while (inb(SERIAL_UART + 5) & 0x01) tty_insert_flip_char(tty, inb(SERIAL_UART));
    tty_flip_buffer_push(tty);

    intc_eoi();
}

/* the uart as the console tty. once the io apic is up */
void serial_tty_init(void) {
    struct tty_struct *tty = tty_alloc(&uart_ops, NULL);
    if (!tty) panic("serial: unable to allocate the tty -- out of memory?");

    console_tty = tty;

    int err = irq_request(4, uart_irq, tty);
    if (err) panic("serial: unable to route irq 4 -- error %d", err);

    outb(SERIAL_UART + 1, 0x01); /* interrupt on received data */
}
