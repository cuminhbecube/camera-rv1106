#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

static int gpio_export(int pin) {
    char path[64];
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) return -1;
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", pin);
    if (write(fd, buf, len) != len) { close(fd); return -1; }
    close(fd);
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", pin);
    // Wait briefly for sysfs to create directory
    for (int i=0; i<50; ++i) {
        struct stat st; if (stat(path, &st)==0) return 0; usleep(20000);
    }
    return -1;
}

static int gpio_set_direction(int pin, const char *dir) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    if (write(fd, dir, strlen(dir)) != (ssize_t)strlen(dir)) { close(fd); return -1; }
    close(fd);
    return 0;
}

static int gpio_write(int pin, int value) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    const char *v = value ? "1" : "0";
    if (write(fd, v, 1) != 1) { close(fd); return -1; }
    close(fd);
    return 0;
}

static void print_cpu_info(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) { perror("cpuinfo"); return; }
    printf("=== CPU Info (truncated) ===\n");
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), f) && count < 40) {
        fputs(line, stdout);
        count++;
    }
    fclose(f);
}

static void peripheral_stub(const char *name) {
    printf("[STUB] %s test not implemented yet.\n", name);
}

static void blink_led(int pin, int count, int delay_ms) {
    if (pin < 0) { printf("No LED pin specified; skipping blink.\n"); return; }
    if (gpio_export(pin) != 0) { fprintf(stderr, "GPIO export failed; skipping.\n"); return; }
    if (gpio_set_direction(pin, "out") != 0) { fprintf(stderr, "Set direction failed; skipping.\n"); return; }
    printf("Blinking GPIO %d %d times...\n", pin, count);
    for (int i=0; i<count; ++i) {
        gpio_write(pin, 1); usleep(delay_ms * 1000);
        gpio_write(pin, 0); usleep(delay_ms * 1000);
    }
    printf("Blink complete.\n");
}

int main(int argc, char **argv) {
    int led_pin = -1;
    int blink_count = 5;
    int delay_ms = 250;
    int do_blink = 0;
    int do_i2c = 0, do_spi = 0, do_uart = 0;

    for (int i=1; i<argc; ++i) {
        if (!strcmp(argv[i], "--blink") && i+1 < argc) { led_pin = atoi(argv[++i]); do_blink = 1; }
        else if (!strcmp(argv[i], "--count") && i+1 < argc) { blink_count = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--delay-ms") && i+1 < argc) { delay_ms = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--i2c-test")) { do_i2c = 1; }
        else if (!strcmp(argv[i], "--spi-test")) { do_spi = 1; }
        else if (!strcmp(argv[i], "--uart-test")) { do_uart = 1; }
        else if (!strcmp(argv[i], "--help")) {
            printf("Luckfox Pico Pro Max Test Utility (RV1106)\n");
            printf("Usage: %s [OPTIONS]\n\n", argv[0]);
            printf("Options:\n");
            printf("  --blink <pin>    GPIO pin number for LED blink (e.g. 73 for GPIO2_B1)\n");
            printf("  --count <N>      Blink count (default: 5)\n");
            printf("  --delay-ms <D>   Delay between toggles in ms (default: 250)\n");
            printf("  --i2c-test       Run I2C stub\n");
            printf("  --spi-test       Run SPI stub\n");
            printf("  --uart-test      Run UART stub\n");
            printf("  --help           Show this help\n\n");
            printf("Environment:\n");
            printf("  LED_PIN=<pin>    Alias for --blink\n\n");
            printf("Example GPIO (verify your schematic):\n");
            printf("  GPIO2_B1 = 73, GPIO2_B0 = 72, GPIO0_A3 = 3\n");
            return 0;
        }
    }

    const char *env_led = getenv("LED_PIN");
    if (env_led && led_pin < 0) { led_pin = atoi(env_led); do_blink = 1; }

    printf("=== Luckfox Pico Pro Max Test (RV1106) ===\n");
    print_cpu_info();

    if (do_blink) blink_led(led_pin, blink_count, delay_ms);
    if (do_i2c) peripheral_stub("I2C");
    if (do_spi) peripheral_stub("SPI");
    if (do_uart) peripheral_stub("UART");

    printf("\nTest complete.\n");
    return 0;
}
