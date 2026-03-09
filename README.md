# Simple_Pi
Bridging the Gap: High-Performance Computing on Mobile Hardware
Overview
Simply Pi is a high-performance C++ program designed to calculate millions of digits of Pi on mobile devices (Android). What started as a joke to surprise a math teacher evolved into a serious case study in optimizing high-precision arithmetic for constrained hardware.

Using the world-famous Chudnovsky Algorithm, this program brings "desktop-class" calculation capabilities to mid-range smartphones.
 Key Features & Optimizations
Calculating Pi on a phone is limited by heat, battery, and memory. This project overcomes those limits with:

Chudnovsky Algorithm: The fastest known method for calculating Pi (converges at ~14 digits per term).
Binary Splitting: Implements "Divide and Conquer" strategies to break complex math into manageable chunks, reducing calculation complexity.
GMP State Serialization: A custom solution to save raw binary data instead of text. This prevents massive storage usage and speeds up the I/O process.
Multi-threading: Utilizes all available CPU cores to double calculation speeds.
The Experiment
This program was tested on a Xiaomi Redmi 12 (MediaTek Helio G88) using a custom-engineered cooling solution involving aluminum foil and active airflow.

Achievement: Successfully calculated ~400 million digits over a period of several days (non-continuous running).
Stability: The program features a "Checkpoint System" to save progress, allowing the calculation to survive battery drain, crashes, or thermal throttling.
Disclaimer & Warranty
THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

This program is designed to push hardware to its absolute limits. Running this software:

May cause your device to heat up significantly.
May result in thermal throttling or system instability.
Could potentially shorten the lifespan of your battery or hardware if adequate cooling is not provided.
I the author is NOT responsible for any hardware damage, data loss, or other damages resulting from the use of this software. Use at your own risk.

License
This project is licensed under the Apache 2.0 License - see the LICENSE file for details.
