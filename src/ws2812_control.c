#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>

// RV1106 GPIO1 Base Address (Physical)
#define GPIO1_BASE_PHY 0xFF4B0000
#define MAP_SIZE 4096UL
#define MAP_MASK (MAP_SIZE - 1)

// GPIO Controller Version Selection
// Uncomment the following line if RV1106 uses the "V2" GPIO controller (likely for newer Rockchip SoCs)
// V2 supports atomic set/clear via high 16-bits mask.
#define GPIO_V2_CONTROLLER 1

#ifdef GPIO_V2_CONTROLLER
    // Offsets for V2
    #define GPIO_SWPORT_DR_L  0x0000
    #define GPIO_SWPORT_DR_H  0x0004
    #define GPIO_SWPORT_DDR_L 0x0008
    #define GPIO_SWPORT_DDR_H 0x000C
#else
    // Offsets for V1 (Standard DesignWare/Older Rockchip)
    #define GPIO_SWPORT_DR    0x0000
    #define GPIO_SWPORT_DDR   0x0004
#endif

// GPIO1_C6 is Pin 22 (Bank C, Index 6) -> 16 + 6 = 22
#define GPIO_PIN_OFFSET 22

// For V2, we need to know if it's in the Low (0-15) or High (16-31) bank.
// 22 is in High bank. Bit index within High bank is 22 - 16 = 6.
#define GPIO_V2_BANK_IS_HIGH 1
#define GPIO_V2_BIT_INDEX    6

// Timing constants (approximate loop iterations - NEEDS CALIBRATION)
// WS2812 Timing:
// T0H: 0.4us
// T0L: 0.85us
// T1H: 0.8us
// T1L: 0.45us
// Total: 1.25us
//
// You must tune these values with an oscilloscope or logic analyzer!
#define DELAY_T0H  5
#define DELAY_T0L  15
#define DELAY_T1H  15
#define DELAY_T1L  5
#define DELAY_RESET 2000 // > 50us

static volatile uint32_t *gpio_base;

static inline void busy_wait(int count) {
    volatile int i;
    for (i = 0; i < count; i++) {
        __asm__ __volatile__("nop");
    }
}

void gpio_setup() {
    int mem_fd;
    void *mapped_base;

    if ((mem_fd = open("/dev/mem", O_RDWR | O_SYNC)) == -1) {
        perror("Can't open /dev/mem");
        exit(1);
    }

    mapped_base = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, GPIO1_BASE_PHY & ~MAP_MASK);
    if (mapped_base == (void *) -1) {
        perror("Can't mmap /dev/mem");
        exit(1);
    }

    gpio_base = (volatile uint32_t *)((char *)mapped_base + (GPIO1_BASE_PHY & MAP_MASK));
    close(mem_fd);

    // Configure Direction to Output
#ifdef GPIO_V2_CONTROLLER
    // Atomic write to DDR_H: Set bit 6 to 1 (Output)
    // Format: (Mask << 16) | Value
    // Mask bit: 1 << (6 + 16) -> 1 << 22
    // Value bit: 1 << 6
    uint32_t val = (1 << (GPIO_V2_BIT_INDEX + 16)) | (1 << GPIO_V2_BIT_INDEX);
    *(gpio_base + (GPIO_SWPORT_DDR_H / 4)) = val;
#else
    // Read-Modify-Write for V1
    uint32_t current = *(gpio_base + (GPIO_SWPORT_DDR / 4));
    *(gpio_base + (GPIO_SWPORT_DDR / 4)) = current | (1 << GPIO_PIN_OFFSET);
#endif
}

static inline void set_pin_high() {
#ifdef GPIO_V2_CONTROLLER
    // Atomic Set High
    *(gpio_base + (GPIO_SWPORT_DR_H / 4)) = (1 << (GPIO_V2_BIT_INDEX + 16)) | (1 << GPIO_V2_BIT_INDEX);
#else
    // RMW High (Slow!)
    *(gpio_base + (GPIO_SWPORT_DR / 4)) |= (1 << GPIO_PIN_OFFSET);
#endif
}

static inline void set_pin_low() {
#ifdef GPIO_V2_CONTROLLER
    // Atomic Set Low
    *(gpio_base + (GPIO_SWPORT_DR_H / 4)) = (1 << (GPIO_V2_BIT_INDEX + 16)) | (0 << GPIO_V2_BIT_INDEX);
#else
    // RMW Low (Slow!)
    *(gpio_base + (GPIO_SWPORT_DR / 4)) &= ~(1 << GPIO_PIN_OFFSET);
#endif
}

void send_byte(uint8_t byte) {
    for (int i = 0; i < 8; i++) {
        if (byte & 0x80) {
            // Send 1
            set_pin_high();
            busy_wait(DELAY_T1H);
            set_pin_low();
            busy_wait(DELAY_T1L);
        } else {
            // Send 0
            set_pin_high();
            busy_wait(DELAY_T0H);
            set_pin_low();
            busy_wait(DELAY_T0L);
        }
        byte <<= 1;
    }
}

void send_color(uint8_t r, uint8_t g, uint8_t b) {
    // WS2812 expects GRB order
    send_byte(g);
    send_byte(r);
    send_byte(b);
}

void show() {
    busy_wait(DELAY_RESET);
}

int main(int argc, char **argv) {
    printf("WS2812 Control on GPIO1_C6 (Pin 54)\n");
    printf("WARNING: This requires root privileges and precise timing calibration.\n");

    gpio_setup();

    // Example: Cycle colors
    while (1) {
        // Red
        send_color(255, 0, 0);
        show();
        usleep(500000);

        // Green
        send_color(0, 255, 0);
        show();
        usleep(500000);

        // Blue
        send_color(0, 0, 255);
        show();
        usleep(500000);
    }

    return 0;
}
