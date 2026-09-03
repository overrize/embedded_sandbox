#include "mock_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MOCK_GPIO_PIN_COUNT 8
#define MOCK_MAX_LIVE_ALLOCS 256

static int      s_gpio_level[MOCK_GPIO_PIN_COUNT];
static int      s_log_count;
static clock_t  s_start_clock;

typedef struct {
    void  *ptr;
    size_t size;
} live_alloc_t;

static live_alloc_t s_live[MOCK_MAX_LIVE_ALLOCS];
static int           s_live_count;

static void mock_log(const char *msg)
{
    s_log_count++;
    printf("[module log] %s\n", msg);
}

static int mock_gpio_set(int pin, int level)
{
    if (pin < 0 || pin >= MOCK_GPIO_PIN_COUNT) {
        printf("[mock] gpio_set(%d, %d) REJECTED -- pin not in mock's whitelist range [0,%d)\n",
               pin, level, MOCK_GPIO_PIN_COUNT);
        return -1;
    }
    s_gpio_level[pin] = level ? 1 : 0;
    printf("[mock] gpio_set(%d, %d)\n", pin, s_gpio_level[pin]);
    return 0;
}

static int mock_gpio_get(int pin)
{
    if (pin < 0 || pin >= MOCK_GPIO_PIN_COUNT) {
        printf("[mock] gpio_get(%d) REJECTED -- pin not in mock's whitelist range [0,%d)\n",
               pin, MOCK_GPIO_PIN_COUNT);
        return -1;
    }
    printf("[mock] gpio_get(%d) -> %d\n", pin, s_gpio_level[pin]);
    return s_gpio_level[pin];
}

static void mock_delay_ms(uint32_t ms)
{
    /* No real sleep -- mock_host's whole point is fast iteration, and a
     * module blocking on a real delay here would just make every test
     * run slower for zero extra bug-catching value. Logged so a module
     * calling delay_ms(huge_number) is still visible in the output. */
    printf("[mock] delay_ms(%u) -- not actually slept\n", (unsigned)ms);
}

static void *mock_alloc(size_t n)
{
    void *p = malloc(n);
    if (p != NULL && s_live_count < MOCK_MAX_LIVE_ALLOCS) {
        s_live[s_live_count].ptr = p;
        s_live[s_live_count].size = n;
        s_live_count++;
    }
    printf("[mock] alloc(%zu) -> %p\n", n, p);
    return p;
}

static void mock_free(void *p)
{
    printf("[mock] free(%p)\n", p);
    if (p == NULL) {
        return;
    }
    for (int i = 0; i < s_live_count; i++) {
        if (s_live[i].ptr == p) {
            s_live[i] = s_live[s_live_count - 1];
            s_live_count--;
            free(p);
            return;
        }
    }
    printf("[mock] WARNING: free(%p) -- not a live pointer this mock handed out "
           "(double-free, or a pointer alloc() never returned)\n", p);
    /* Deliberately do NOT call the real free() here -- forwarding an
     * invalid pointer to the platform allocator is exactly the kind of
     * crash mock_host exists to catch cleanly, not suppress. */
}

static uint32_t mock_uptime_ms(void)
{
    clock_t elapsed = clock() - s_start_clock;
    return (uint32_t)(elapsed * 1000 / CLOCKS_PER_SEC);
}

static const host_api_t s_mock_api = {
    .abi_ver   = HOST_API_ABI_VERSION,
    .log       = mock_log,
    .gpio_set  = mock_gpio_set,
    .gpio_get  = mock_gpio_get,
    .delay_ms  = mock_delay_ms,
    .alloc     = mock_alloc,
    .free      = mock_free,
    .uptime_ms = mock_uptime_ms,
};

const host_api_t *mock_api_get(void)
{
    s_start_clock = clock();
    return &s_mock_api;
}

void mock_api_print_summary(void)
{
    printf("\n[mock summary] %d log line(s), %d live allocation(s) at exit",
           s_log_count, s_live_count);
    if (s_live_count > 0) {
        printf(" -- LEAK:");
        for (int i = 0; i < s_live_count; i++) {
            printf(" %p(%zuB)", s_live[i].ptr, s_live[i].size);
        }
    }
    printf("\n[mock summary] gpio pin states:");
    for (int i = 0; i < MOCK_GPIO_PIN_COUNT; i++) {
        printf(" %d=%d", i, s_gpio_level[i]);
    }
    printf("\n");
}
