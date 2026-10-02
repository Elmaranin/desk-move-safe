#ifndef LED_H
#define LED_H
//
// led — the onboard WS2812 on GP16, the board's only LED.
//
// One job for now: a panel press the desk lock refused blinks it red, so the
// person at the desk can see the board heard them and is refusing on purpose —
// rather than a desk that simply looks broken. Otherwise it is off.
//
// led_refused() is called from the bus task and only stores a tick; the
// pattern is played by led_task at the lowest priority.
//
void led_init(void);
void led_task(void *arg);
void led_refused(void);

#endif // LED_H
