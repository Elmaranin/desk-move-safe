//
// trace — see trace.h.
//
#include "trace.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>

#define TRACE_DEPTH 128             // ~10 s of a busy move at panel rate

typedef struct {
    uint32_t ms;
    uint8_t  type;
    int32_t  a, b;
} ev_t;

static ev_t              s_ring[TRACE_DEPTH];
static volatile uint32_t s_head, s_tail;    // head written by producers, tail by the console
static volatile uint32_t s_lost;
static volatile bool     s_on;
static uint32_t          s_t0;              // ms at 'debug start'

static uint32_t now_ms(void) { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

void trace_set(bool on)
{
    taskENTER_CRITICAL();
    s_head = s_tail = 0;
    s_lost = 0;
    s_t0   = now_ms();
    s_on   = on;
    taskEXIT_CRITICAL();
}

bool trace_on(void) { return s_on; }

void trace_add(tr_type_t type, int32_t a, int32_t b)
{
    if (!s_on) return;
    taskENTER_CRITICAL();
    if (s_head - s_tail >= TRACE_DEPTH) {
        s_lost++;
    } else {
        ev_t *e = &s_ring[s_head % TRACE_DEPTH];
        e->ms = now_ms(); e->type = (uint8_t)type; e->a = a; e->b = b;
        s_head++;
    }
    taskEXIT_CRITICAL();
}

// ---- formatting ------------------------------------------------------------

static const char *state_name(int32_t s)
{
    static const char *n[] = { "PASS", "PLANNING", "STOPPING", "MOVING" };
    return (s >= 0 && s < 4) ? n[s] : "?";
}

static const char *key_name(int32_t k)
{
    switch (k) {
        case 0x00: return "idle";
        case 0x01: return "STAND recall";
        case 0x02: return "SIT recall";
        case 0x03: return "save stand";
        case 0x04: return "save sit";
        case 0x05: return "DOWN edge";
        case 0x06: return "DOWN held";
        case 0x07: return "UP edge";
        case 0x08: return "UP held";
        case 0x0E: return "panel wake";
        case 0xFF: return "nothing (frame dropped)";
    }
    return "unknown";
}

static void print(const ev_t *e)
{
    uint32_t t = e->ms - s_t0;
    printf("[dbg %4lu.%03lu] ", (unsigned long)(t / 1000), (unsigned long)(t % 1000));
    switch (e->type) {
    case TR_STATE:
        if (e->a == e->b) printf("state  %s\n", state_name(e->a));
        else              printf("state  %s -> %s\n", state_name(e->a), state_name(e->b));
        break;
    case TR_PANEL:
        printf("panel  sends 0x%02lX %s\n", (unsigned long)e->a, key_name(e->a));
        break;
    case TR_SENT:
        printf("board  hears 0x%02lX %s\n", (unsigned long)e->a, key_name(e->a));
        break;
    case TR_HEIGHT:
        printf("height %ld mm%s\n", (long)e->a,
               e->b > 0 ? " (going up)" : e->b < 0 ? " (going down)" : "");
        break;
    case TR_TARGET:
        if (e->a) printf("board  announces destination %ld mm\n", (long)e->a);
        else      printf("board  announcement gone\n");
        break;
    case TR_FLAG: {
        static const char *f[] = { "hands-off", "armed", "desk locked", "intercept" };
        printf("flag   %s = %s\n", (e->a >= 0 && e->a < 4) ? f[e->a] : "?",
               e->b ? "yes" : "no");
        break;
    }
    case TR_JOB:
        switch (e->a) {
        case TRJ_TAKE:       printf("job    flap takeover, destination %ld mm\n", (long)e->b); break;
        case TRJ_GO:         printf("job    'go' to %ld mm\n", (long)e->b); break;
        case TRJ_AT_FLAP:    printf("job    %s\n", e->b ? "at the flap height" : "did NOT reach the flap height"); break;
        case TRJ_FLAP_START: printf("job    flap running\n"); break;
        case TRJ_FLAP_END:   printf("job    flap done\n"); break;
        case TRJ_RESUME:     printf("job    resume: re-sending %s once\n", key_name(e->b)); break;
        case TRJ_SENT:       printf("job    %s\n", e->b ? "recall went out on the wire"
                                                         : "recall NEVER went out (panel quiet)"); break;
        case TRJ_ABORTED:    printf("job    aborted ('stop')\n"); break;
        case TRJ_DONE:       printf("job    done — bus handed back to the panel\n"); break;
        default:             printf("job    phase %ld\n", (long)e->a); break;
        }
        break;
    case TR_RECALL: {
        static const char *why[] = {
            "crosses the flap — taken over before the board hears it",
            "does not cross the flap — passed through",
            "destination not known yet — passed through, waiting for the board",
            "announced, crosses the flap — stopping it there",
            "announced, does not cross — left alone",
            "no destination announced in time — left alone",
            "PASSED: the flap stop is off (desk_flap_on 0)",
            "PASSED: hands-off after a flap move",
            "PASSED: a flap job already owns the desk",
            "PASSED: dev mode, or the desk/flap check failed",
            "PASSED: no fresh height to judge it by",
            "caught by the approach stop — flap, then the recall again",
        };
        printf("recall %s: %s\n", key_name(e->a),
               (e->b >= 0 && e->b < (int32_t)(sizeof why / sizeof why[0])) ? why[e->b] : "?");
        break;
    }
    default:
        printf("event %u %ld %ld\n", e->type, (long)e->a, (long)e->b);
    }
}

void trace_drain(void)
{
    uint32_t lost = s_lost;
    if (lost) {
        s_lost = 0;
        printf("[dbg] %lu events LOST — the console could not keep up\n", (unsigned long)lost);
    }
    // At most a screenful per call, so the console stays responsive.
    for (int n = 0; n < 16 && s_tail != s_head; n++) {
        ev_t e = s_ring[s_tail % TRACE_DEPTH];
        s_tail++;
        print(&e);
    }
}

void trace_report(void)
{
    printf("debug trace %s | %lu events waiting\n", s_on ? "ON" : "off",
           (unsigned long)(s_head - s_tail));
}
