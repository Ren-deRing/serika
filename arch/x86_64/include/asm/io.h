#pragma once

#include <stdint.h>

static void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm __volatile__ ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
