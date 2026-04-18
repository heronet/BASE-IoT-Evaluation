/*
 * BASE Concurrent Workload Test
 *
 * Measures deadline adherence of a high-priority control task (tau1)
 * while a low-priority biometric task (tau2) runs a 500ms capture.
 *
 * Two modes selected at compile time:
 *   CONFIG_TEST_LEGACY_BLOCKING=y  -> tau2 uses k_sched_lock + k_busy_wait
 *                                     (simulates non-preemptible vendor driver)
 *   CONFIG_TEST_LEGACY_BLOCKING=n  -> tau2 uses biometric_enroll_capture
 *                                     (BASE hybrid yielding model)
 *
 * Output over UART:
 *   [RESULT] tau1 periods: N samples, max_jitter=Xus, missed=Y
 */

#include <zephyr/device.h>
#include <zephyr/drivers/biometrics.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/timing/timing.h>

LOG_MODULE_REGISTER(base_test, LOG_LEVEL_INF);

/* tau1: high-priority control loop, period 10ms, deadline 10ms */
#define TAU1_PERIOD_US 10000U
#define TAU1_DEADLINE_US 10000U
#define TAU1_STACK_SIZE 1024
#define TAU1_PRIORITY 2

/* tau2: low-priority biometric task */
#define TAU2_STACK_SIZE 2048
#define TAU2_PRIORITY 8

/* Number of tau1 periods to measure */
#define TAU1_SAMPLE_COUNT 200

/* Template ID used for the test enrollment */
#define TEST_TEMPLATE_ID 1U

/* Sensor device alias */
#define BIOMETRIC_NODE DT_ALIAS(biometric0)

/* Shared state */

static volatile bool test_done;
static volatile bool tau2_done;

static uint32_t tau1_jitter_us[TAU1_SAMPLE_COUNT];
static uint32_t tau1_missed;
static uint32_t tau1_sample_idx;

/* tau1 signals tau2 to start once tau1 is running */
K_SEM_DEFINE(tau2_start_sem, 0, 1);

/* tau1: high-priority periodic control task */

K_THREAD_STACK_DEFINE(tau1_stack, TAU1_STACK_SIZE);
static struct k_thread tau1_thread;

static void tau1_entry(void *p1, void *p2, void *p3) {
  ARG_UNUSED(p1);
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  uint64_t prev_us = k_ticks_to_us_near64(k_uptime_ticks());

  /* Signal tau2 to start now that tau1 is measuring */
  k_sem_give(&tau2_start_sem);

  while (tau1_sample_idx < TAU1_SAMPLE_COUNT) {
    k_usleep(TAU1_PERIOD_US);

    uint64_t now_us = k_ticks_to_us_near64(k_uptime_ticks());
    uint64_t actual_period = now_us - prev_us;

    prev_us = now_us;

    uint32_t jitter = (actual_period > TAU1_PERIOD_US)
                          ? (uint32_t)(actual_period - TAU1_PERIOD_US)
                          : 0U;

    tau1_jitter_us[tau1_sample_idx] = jitter;

    if (actual_period >= TAU1_DEADLINE_US + 1000U) {
      tau1_missed++;
    }

    tau1_sample_idx++;
  }

  test_done = true;
}

/* tau2: low-priority biometric task */

K_THREAD_STACK_DEFINE(tau2_stack, TAU2_STACK_SIZE);
static struct k_thread tau2_thread;

static void tau2_entry(void *p1, void *p2, void *p3) {
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  const struct device *dev = (const struct device *)p1;

  /* Wait until tau1 is measuring */
  k_sem_take(&tau2_start_sem, K_FOREVER);

#if defined(CONFIG_TEST_LEGACY_BLOCKING) && CONFIG_TEST_LEGACY_BLOCKING
  /*
   * Legacy baseline: k_sched_lock() plus k_busy_wait() models a
   * polling driver that holds scheduler lock while waiting on UART data.
   * On this single-core test setup, tau1 cannot run during the 500 ms
   * locked busy-wait window.
   *
   * Note: raw k_busy_wait() alone does not reliably model this legacy
   * behavior because it may still be preempted by normal scheduling.
   */
  LOG_INF("tau2: running LEGACY non-preemptible busy-wait mode");

  while (!test_done) {
    k_sched_lock();
    k_busy_wait(500000U); /* 500ms non-preemptible spin */
    k_sched_unlock();
    k_sleep(K_MSEC(10));
  }

#else
  /*
   * BASE mode: biometric_enroll_capture() yields the CPU to the
   * scheduler throughout the capture window via cooperative
   * k_msleep polling and semaphore-based UART synchronization.
   * tau1 runs freely during the entire capture window.
   */
  LOG_INF("tau2: running BASE hybrid yielding mode");

  int ret;

  /* Clean up any leftover template from a previous run */
  biometric_template_delete(dev, TEST_TEMPLATE_ID);

  while (!test_done) {
    struct biometric_capture_result cap = {0};

    ret = biometric_enroll_start(dev, TEST_TEMPLATE_ID);
    if (ret < 0 && ret != -EBUSY) {
      LOG_WRN("enroll_start: %d", ret);
      k_sleep(K_MSEC(100));
      continue;
    }

    /*
     * Capture loop: adapts to samples_required automatically.
     * Works for ZFM (2 samples), GT-5X (3 samples), AI10 (1).
     */
    do {
      ret = biometric_enroll_capture(dev, K_MSEC(600), &cap);
      if (ret < 0) {
        LOG_DBG("capture returned: %d", ret);
        break;
      }
    } while (cap.samples_captured < cap.samples_required);

    if (ret == 0) {
      biometric_enroll_finalize(dev);
    } else {
      biometric_enroll_abort(dev);
    }

    /* Brief pause between capture attempts */
    k_sleep(K_MSEC(50));

    /* Remove template to keep storage clean */
    biometric_template_delete(dev, TEST_TEMPLATE_ID);
  }
#endif /* CONFIG_TEST_LEGACY_BLOCKING */

  tau2_done = true;
}

/* Result reporting */

static void print_results(void) {
  uint32_t max_jitter = 0U;
  uint32_t total_jitter = 0U;

  for (uint32_t i = 0; i < TAU1_SAMPLE_COUNT; i++) {
    if (tau1_jitter_us[i] > max_jitter) {
      max_jitter = tau1_jitter_us[i];
    }
    total_jitter += tau1_jitter_us[i];
  }

  uint32_t avg_jitter = total_jitter / TAU1_SAMPLE_COUNT;

#if defined(CONFIG_TEST_LEGACY_BLOCKING) && CONFIG_TEST_LEGACY_BLOCKING
  const char *mode = "LEGACY (non-preemptible busy-wait)";
#else
  const char *mode = "BASE (hybrid yielding)";
#endif

  printk("\n========================================\n");
  printk(" BASE Concurrent Workload Test Results\n");
  printk("========================================\n");
  printk(" Mode             : %s\n", mode);
  printk(" tau1 samples     : %u\n", TAU1_SAMPLE_COUNT);
  printk(" tau1 period      : %u us\n", TAU1_PERIOD_US);
  printk(" tau1 deadline    : %u us\n", TAU1_DEADLINE_US);
  printk(" Max jitter       : %u us\n", max_jitter);
  printk(" Avg jitter       : %u us\n", avg_jitter);
  printk(" Missed deadlines : %u / %u\n", tau1_missed, TAU1_SAMPLE_COUNT);
  printk("========================================\n\n");

  printk("CSV: mode,max_jitter_us,avg_jitter_us,missed\n");
  printk("CSV: %s,%u,%u,%u\n", mode, max_jitter, avg_jitter, tau1_missed);
}

int main(void) {
  const struct device *bio_dev = DEVICE_DT_GET(BIOMETRIC_NODE);

  if (!device_is_ready(bio_dev)) {
    printk("ERROR: biometric device not ready\n");
    return -ENODEV;
  }

  printk("BASE concurrent workload test starting\n");

#if defined(CONFIG_TEST_LEGACY_BLOCKING) && CONFIG_TEST_LEGACY_BLOCKING
  printk("Mode: LEGACY non-preemptible busy-wait baseline\n");
#else
  printk("Mode: BASE hybrid yielding\n");
#endif

  timing_init();
  timing_start();

  tau2_done = false;
  test_done = false;
  tau1_missed = 0U;
  tau1_sample_idx = 0U;

  /* Start tau2 first at lower priority */
  k_thread_create(&tau2_thread, tau2_stack, K_THREAD_STACK_SIZEOF(tau2_stack),
                  tau2_entry, (void *)bio_dev, NULL, NULL, TAU2_PRIORITY, 0,
                  K_NO_WAIT);
  k_thread_name_set(&tau2_thread, "tau2_biometric");

  /* Start tau1 at higher priority, it signals tau2 when ready */
  k_thread_create(&tau1_thread, tau1_stack, K_THREAD_STACK_SIZEOF(tau1_stack),
                  tau1_entry, NULL, NULL, NULL, TAU1_PRIORITY, 0, K_NO_WAIT);
  k_thread_name_set(&tau1_thread, "tau1_control");

  /* Wait for tau1 to finish its sample window */
  k_thread_join(&tau1_thread, K_SECONDS(30));

  /* tau2 will notice test_done and exit */
  k_thread_join(&tau2_thread, K_SECONDS(5));

  timing_stop();

  print_results();

  return 0;
}