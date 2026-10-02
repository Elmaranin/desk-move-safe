#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

// Single-core FreeRTOS config for the RP2350 (Cortex-M33, secure build).
//
// Single-core (configNUMBER_OF_CORES = 1) because nothing here needs a second
// one: the only hard-real-time work is the step-pulse alarm ISR, and an ISR
// beats a task on either core. One core also keeps the timing story simple.

// ---- core count ----------------------------------------------------------
#define configNUMBER_OF_CORES                   1

// ---- Cortex-M33 port (RP2350, non-TrustZone) -----------------------------
#define configENABLE_FPU                        1
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
// RP2350 with PICO_PLATFORM=rp2350-arm-s boots and stays in the SECURE state,
// so FreeRTOS must run secure-only. Without this the ARMv8-M port builds its
// context switch for the non-secure side and the scheduler faults on task 1.
#define configRUN_FREERTOS_SECURE_ONLY          1
#define configMINIMAL_SECURE_STACK_SIZE         1024

// Interrupt priority threshold for kernel-safe ISRs. RP2350's M33 uses the top
// nibble for priority; 16 == priority level 1. The step alarm runs at the SDK
// default (0x80), which is numerically above this, so it is both maskable by
// taskENTER_CRITICAL and allowed to call the FromISR API — stepper.c relies on
// exactly that.
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16

// Let FreeRTOS objects interoperate with Pico SDK sync/time primitives, so the
// SDK's sleep_ms()/stdio yield to the scheduler instead of busy-waiting.
#define configSUPPORT_PICO_SYNC_INTEROP         1
#define configSUPPORT_PICO_TIME_INTEROP         1

// ---- scheduler -----------------------------------------------------------
#define configUSE_PREEMPTION                    1
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      150000000
#define configTICK_RATE_HZ                      1000
#define configMAX_PRIORITIES                    32
#define configMINIMAL_STACK_SIZE                256
#define configTOTAL_HEAP_SIZE                   (64 * 1024)
#define configMAX_TASK_NAME_LEN                 16
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TIME_SLICING                  1
#define configSTACK_DEPTH_TYPE                  uint32_t

// ---- synchronisation primitives -----------------------------------------
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_QUEUE_SETS                    1
#define configQUEUE_REGISTRY_SIZE               8

// ---- memory --------------------------------------------------------------
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configUSE_NEWLIB_REENTRANT              0

// ---- assert --------------------------------------------------------------
// Spin on a failed assertion so it can be caught in the debugger.
// An assert used to spin silently, which is indistinguishable from any other
// hang. Now it says where it came from first. vAssertCalled is in main.c.
void vAssertCalled(const char *file, int line);
#define configASSERT(x) if ((x) == 0) vAssertCalled(__FILE__, __LINE__)

// ---- hooks / checks ------------------------------------------------------
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            0
// Method 2: paint the stack at creation and check the pattern on every
// context switch. Costs a few microseconds per switch and turns "the console
// stopped and nothing else happened" — which is what a blown stack looks like
// — into a printed task name. Worth it permanently on a rig this size.
#define configCHECK_FOR_STACK_OVERFLOW          2

// ---- software timers -----------------------------------------------------
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            1024

// ---- optional API --------------------------------------------------------
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xResumeFromISR                  1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_xSemaphoreGetMutexHolder        1

#endif // FREERTOS_CONFIG_H
