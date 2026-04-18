BASE Concurrent Workload Test
######################################

Overview
********

This application measures how well a high-priority periodic control task keeps
its timing guarantees while a low-priority biometric workload runs concurrently.

It compares two modes:

#. **LEGACY (busy-wait baseline)**
   The biometric task simulates a non-preemptible polling driver using
   ``k_sched_lock()`` followed by ``k_busy_wait(500000)``. Without the
   scheduler lock, Zephyr's preemptive scheduler would still allow
   ``tau1`` to run, which would not reproduce legacy blocking behavior.
#. **BASE (hybrid yielding)**
   The biometric task uses Zephyr biometrics APIs (enroll start/capture/finalize),
   allowing the scheduler to run other threads during sensor latency.

The test reports jitter and missed deadlines for the control loop.

What It Measures
****************

- ``tau1`` (high priority): control loop with 10 ms period and 10 ms deadline
  measured over 200 samples.
- ``tau2`` (low priority): biometric capture workload that runs while ``tau1``
  is being measured.
- Output metrics:

  - max jitter (us)
  - average jitter (us)
  - missed deadlines

At the end, the app prints both a human-readable summary and a CSV line.

Hardware and Devicetree Requirements
************************************

This repository currently includes board overlay support for:

- ``weact_stm32f446_core``

The overlay expects a fingerprint sensor compatible with
``zhiantec,zfm-x0`` connected on ``usart2`` and exposed as devicetree alias
``biometric0``.

If ``biometric0`` is missing or the device is not ready, startup fails with:

.. code-block:: text

   ERROR: biometric device not ready

Configuration
*************

The mode is selected by ``CONFIG_TEST_LEGACY_BLOCKING``:

- ``y``: run LEGACY busy-wait baseline
- ``n``: run BASE hybrid yielding biometric flow

The option is defined in ``Kconfig`` and currently set in ``prj.conf``.

Building
********

From the project root:

.. code-block:: bash

   west build -b weact_stm32f446_core -p

Flashing and Console
********************

Flash with your normal Zephyr flow for the board/toolchain, for example:

.. code-block:: bash

   west flash

Then open the console/RTT log to view test output.

Expected Output
***************

The app prints a report like:

.. code-block:: text

   BASE concurrent workload test starting
   Mode: LEGACY busy-wait baseline
   ========================================
    BASE Concurrent Workload Test Results
   ========================================
    Mode          : LEGACY (busy-wait)
    tau1 samples  : 200
    tau1 period   : 10000 us
    tau1 deadline : 10000 us
    Max jitter    : <value> us
    Avg jitter    : <value> us
    Missed deadlines: <value> / 200
   ========================================
   CSV: mode,max_jitter_us,avg_jitter_us,missed
   CSV: LEGACY (busy-wait),<max>,<avg>,<missed>

To compare modes, build and run twice, toggling
``CONFIG_TEST_LEGACY_BLOCKING`` between ``y`` and ``n``.
