#ifndef CONSOLE_H
#define CONSOLE_H
//
// console — a small shell over USB CDC.
//
// The firmware runs itself. This exists to answer "what is it doing", to set
// the handful of numbers that belong to this desk rather than to the
// protocol, and to stop it. Everything it offers is something someone
// installing or living with the desk would use; the instrumentation that found
// the protocol in the first place lives in ../desk-lab.
//
#include "FreeRTOS.h"
#include "task.h"

void console_task(void *arg);

#endif // CONSOLE_H
