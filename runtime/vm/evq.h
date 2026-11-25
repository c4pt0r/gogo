/* Event ring: hands peripheral events from interrupt context to the
 * single-threaded scheduler without locks or allocation.
 *
 * One global single-producer / single-consumer ring of GOGO_EVQ_CAP entries
 * (a power of two). The producer (an ISR or other async context) owns the
 * head index and only ever calls evq_push; the consumer (the scheduler)
 * owns the tail index and only ever calls evq_pop. Each side writes only its
 * own index and publishes it after the slot access, ordered with
 * platform_barrier(). When the ring is full the NEW event is dropped and
 * counted (the producer never touches the consumer's index), so the oldest
 * CAP events stay poppable in order.
 */
#ifndef GOGO_EVQ_H
#define GOGO_EVQ_H

#include <stdint.h>
#include <stdbool.h>

typedef struct PortEvent {
    uint16_t port, gen;
    int32_t a, b;
    uint32_t time_ms;
} PortEvent;

bool evq_push(const PortEvent *e);   /* producer (ISR-safe); false + dropped++ when full */
bool evq_pop(PortEvent *out);        /* consumer (scheduler) */
bool evq_empty(void);                /* consumer: nothing to pop right now */
uint32_t evq_dropped(void);
void evq_reset(void);                /* tests and vm start; not concurrent-safe */

#endif
