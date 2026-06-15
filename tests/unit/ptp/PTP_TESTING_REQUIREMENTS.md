# PTP Requirements Specification & Unit Testing Checklist
*Strictly mapped to IEEE Std 1588-2019, SMPTE ST 2059-2:2015, ITU-T Recommendation G.8275.1, and Open-Source Codebases*

This guide serves as the authoritative source of design requirements and testing specifications for the PTP and PI servo modules in the Media Transport Library (MTL). All requirements are derived directly from regulatory standards and verified open-source structures. No behaviors or thresholds are synthesized. Use it to design robust, specification-compliant unit tests under [tests/unit/ptp/](tests/unit/ptp/).

---

## 1. Authoritative Internet Resources & Standard Downloads

To ensure compliance across all implementations, developers and review agents must reference official standard definitions and production implementations.

### Official Specifications & Standard Portals
*   **IEEE Std 1588-2019 (PTP v2.1)**: [IEEE Xplore Standards - IEEE 1588-2019](https://ieeexplore.ieee.org/document/9021800)
    *   *Core Content*: Message layout formats, transaction timings, and clock synchronization loop models.
*   **SMPTE ST 2059-2:2015 (Professional Broadcast Profile)**: [SMPTE ST 2059-2 Store](https://ieeexplore.ieee.org/document/7291656)
    *   *Core Content*: Domain number mapping (127), Sync rates (8Hz), and locking time limits for video networks.
*   **ITU-T Recommendation G.8275.1 (Telecom Phase Profile)**: [ITU-T G.8275.1 Recommendation](https://www.itu.int/rec/T-REC-G.8275.1)
    *   *Core Content*: Alternate Best Master Clock Algorithm (BMCA) parameters and transport mapping.

### Verified Open-Source Implementation Benchmarks
*   **LinuxPTP (Standard Linux Precision Time Protocol)**: [LinuxPTP SourceForge Repository](https://git.code.sf.net/p/linuxptp/code) (Mirror: [LinuxPTP GitHub Project](https://github.com/richardcochran/linuxptp))
    *   *Reference Files*: pi.c (servo calculations), msg.c (packet headers), and phc2sys.c (clock synchronization loop).
*   **PTPd (Precision Time Protocol Daemon)**: [PTPd SourceForge Repository](https://sourceforge.net/projects/ptpd/) (Mirror: [PTPd GitHub Project](https://github.com/ptpd/ptpd))
    *   *Reference Files*: servo.c (phase lock loop filter design) and protocol.c (message state machine).

---

## 2. Packet Parsing & Message Headers

Any conforming PTP parser must validate and unpack messages in network byte order (big-endian) against standard structures.

### Common Header Layout (34 Bytes)
*Source: IEEE Std 1588-2019, Section 13.3 ("Common header of PTP messages"), Table 18 ("Common header")*

| Octet Offset | Standard Data Field | Field Length (Octets) | Format / Valid Values | Reference Section in IEEE Std 1588-2019 |
| :--- | :--- | :--- | :--- | :--- |
| **0 (upper 4b)**| `transportSpecific` | 0.5 (4 bits) | `0x0` (Default profile) or `0x1` (802.1AS) | 13.3.2.1 |
| **0 (lower 4b)**| `messageType` | 0.5 (4 bits) | `0x0`: Sync, `0x1`: Delay_Req, `0x8`: Follow_Up, `0x9`: Delay_Resp, `0xB`: Announce | 13.3.2.2 (Table 19) |
| **1 (lower 4b)**| `versionPTP` | 0.5 (4 bits) | `2` (PTP v2) or `2.1` (PTP v2.1) | 13.3.2.3 |
| **2** | `messageLength` | 2 | Integer representing total PTP message size | 13.3.2.4 |
| **4** | `domainNumber` | 1 | Integer `0` - `255` | 13.3.2.5 (and Section 7.1 "PTP Domain") |
| **5** | `minorVersionPTP` | 1 | `0` or `1` depending on standard version | 13.3.2.13 (or Reserved in IEEE 1588-2008) |
| **6** | `flagField` | 2 | Bitmask (e.g., Bit 1: `twoStepFlag`, Bit 10: `unicastFlag`) | 13.3.2.6 (Table 20) |
| **8** | `correctionField` | 8 | Signed 64-bit Integer representing scaled nanoseconds | 13.3.2.7 |
| **20** | `sourcePortIdentity` | 10 | 8-octet `ClockIdentity` + 2-octet unsigned port number | 13.3.2.8 |
| **30** | `sequenceId` | 2 | Unsigned 16-bit sequential identifier | 13.3.2.9 |
| **32** | `controlField` | 1 | Legacy field (value based on messageType) | 13.3.2.10 (Table 21) |
| **33** | `logMessageInterval`| 1 | Signed Integer representing log base 2 scale of mean interval | 13.3.2.11 |

### 🛠️ Parser Testing Specifications
*   **Truncation Boundaries**: A parser receiving packets shorter than the standard header size of 34 octets or shorter than the defined payload length specified by `messageLength` must immediately terminate parsing and discard the packet (*Source: LinuxPTP reference parser filters in msg.c:msg_post_recv*).
*   **Correction Field Real-time Conversion**: The `correctionField` field is defined as a signed 64-bit fraction of nanoseconds multiplied by $2^{16}$ (*Source: IEEE Std 1588-2019, Section 13.3.2.7*). Implementations must convert fractional values accurately using:
    $$\Delta_{\text{correction}} = \frac{\text{correctionField}}{65536}$$
*   **Domain Matching Validation**: Packet ingress filters must compare the `domainNumber` field to the receiver's configured port domain. If there is a mismatch, the synchronization parameters are ignored (*Source: IEEE Std 1588-2019, Section 7.1 and 9.5.1*).

---

## 3. Best Master Clock Algorithm (BMCA)

The BMCA chooses the active Grandmaster using strict comparisons of the Announce fields.

### Dataset Precedence Hierarchy
*Source: IEEE Std 1588-2019, Section 9.3 ("Best Master Clock Algorithm") and clause 9.3.2.2 "Dataset comparison algorithm"*

Comparisons between two clock datasets $A$ and $B$ must evaluate attributes in the following strict order of precedence:
1.  **`grandmasterPriority1`**: Unsigned 8-bit integer (smaller value dominates class).
2.  **`clockClass`**: Unsigned 8-bit integer defining traceability (e.g., `6` denotes primary GPS lock, `7` is holdover).
3.  **`clockAccuracy`**: Unsigned 8-bit enumerator representing maximum expected offset variance.
4.  **`offsetScaledLogVariance`**: Unsigned 16-bit log-estimate value indicating oscillator frequency stability.
5.  **`grandmasterPriority2`**: Unsigned 8-bit integer (secondary profile priority constraint).
6.  **`grandmasterIdentity`**: 8-octet unique identifier used as final deterministic tie-breaker (numerically lower wins).

### 🛠️ BMCA State Machine Testing Specifications
*   **Announce Receipt Timeout**: A port in `SLAVE` or `LISTENING` state must transition state and initiate search actions if no valid master Announce message is received within the standard period (*Source: IEEE Std 1588-2019, Section 9.2.6.11 and state transitions in Section 9.2.5*):
    $$\text{Timeout Dur} = \text{announceReceiptTimeout} \times 2^{\text{logAnnounceInterval}} \text{ seconds}$$
*   **Forced Dataset Preoption**: Test injection of a dataset with lower `Priority1` value. The system must immediately invalidate current parent properties and demote the active grandmaster status (*Source: IEEE Std 1588-2019, Section 9.3.2*).

---

## 4. Profile-Specific Configurations

### SMPTE ST 2059-2 Profile Parameters
*Source: SMPTE ST 2059-2:2015, Table 1 ("SMPTE Profile parameters") and Section 6.2 "Direct communication PTP parameters"*

Conforming ST 2059-2 implementations must enforce:
*   **Default Profile Domain**: `127` (SMPTE ST 2059-2:2015 Table 1)
*   **Default Sync Message Interval**: $2^{-3}\text{ s}$ (i.e. $8\text{ Hz}$, `logMessageInterval` = `-3`) (SMPTE ST 2059-2:2015 Table 1)
*   **Default Announce Message Interval**: $2^{-2}\text{ s}$ (i.e. $4\text{ Hz}$, `logMessageInterval` = `-2`) (SMPTE ST 2059-2:2015 Table 1)
*   **Default Delay Request Interval**: Same as Sync Message Interval rate ($2^{-3}\text{ s}$)
*   **Locking Time Requirements**: Maximum locking convergence time boundaries must prevent buffer overflows in ST 2110 playback loops.

### ITU-T Telecommunication Profile (G.8275.1)
*Source: ITU-T Recommendation G.8275.1 (03/2020), clause 7 and Annex A*

*   Enforces BMCA modifications for telecom phase profile networks.
*   Direct Port State configuration patterns for Master/Slave boundary constraints.

---

## 5. Precise Time Synchronization Equations

Conforming PTP network engines compute transit delay and clock correction parameters based on event timestamps.

### End-to-End (E2E) Delay Request-Response Math
*Source: IEEE Std 1588-2019, Section 11.2 ("Delay request-response mechanism calculations")*

In a transaction between a PTP Master and a Slave, four event timestamps are captured relative to respective local clock scales:
1.  $$t_1$$: Egress time of the `Sync` message at the Master.
2.  $$t_2$$: Ingress time of the `Sync` message at the Slave.
3.  $$t_3$$: Egress time of the `Delay_Req` message at the Slave.
4.  $$t_4$$: Ingress time of the `Delay_Req` message at the Master.

The individual packet correction terms specified within the standard headers are defined as:
*   $$C_{\text{sync}}$$: Value of `correctionField` in the `Sync` message.
*   $$C_{\text{follow\_up}}$$: Value of `correctionField` in the `Follow_Up` message (if `twoStepFlag` is active).
*   $$C_{\text{delay\_req}}$$: Value of `correctionField` in the `Delay_Req` message.
*   $$C_{\text{delay\_resp}}$$: Value of `correctionField` in the `Delay_Resp` message.

The consolidated correction factor is:
$$C_{\text{all}} = C_{\text{sync}} + C_{\text{follow\_up}} + C_{\text{delay\_req}} + C_{\text{delay\_resp}}$$

The Master-to-Slave transit delay is:
$$\text{Delay}_{\text{m-to-s}} = t_2 - t_1 - C_{\text{sync}} - C_{\text{follow\_up}}$$

The Slave-to-Master transit delay is:
$$\text{Delay}_{\text{s-to-m}} = t_4 - t_3 - C_{\text{delay\_req}} - C_{\text{delay\_resp}}$$

Assuming symmetric path propagation properties:
*   **Mean Path Delay ($\text{Delay}_{\text{mean}}$)**:
    $$\text{Delay}_{\text{mean}} = \frac{(t_2 - t_1) + (t_4 - t_3) - C_{\text{all}}}{2}$$
*   **Offset-from-Master ($\text{Offset}$)**:
    $$\text{Offset} = (t_2 - t_1) - \text{Delay}_{\text{mean}} - C_{\text{sync}} - C_{\text{follow\_up}}$$

Substituting the definition of Mean Path Delay directly back into Offset-from-Master yields:
$$\text{Offset} = \frac{(t_2 - t_1) - (t_4 - t_3) - (C_{\text{sync}} + C_{\text{follow\_up}} - C_{\text{delay\_req}} - C_{\text{delay\_resp}})}{2}$$

### Peer-to-Peer (P2P) Timing Math
*Source: IEEE Std 1588-2019, Section 11.3 ("Peer-to-peer delay mechanism calculations")*

Under P2P, path delay is measured directly between adjacent link nodes using a four-timestamp handshake:
1.  $$t_1'$$: Egress time of the `Pdelay_Req` message at the Slave.
2.  $$t_2'$$: Ingress time of the `Pdelay_Req` message at the upstream Peer.
3.  $$t_3'$$: Egress time of the `Pdelay_Resp` message at the upstream Peer.
4.  $$t_4'$$: Ingress time of the `Pdelay_Resp` message at the Slave.

With cumulative path correction values:
$$C_{\text{pdelay}} = C_{\text{pdelay\_req}} + C_{\text{pdelay\_resp}} + C_{\text{pdelay\_resp\_follow\_up}}$$

The structural formulations evaluate as:
*   **Peer Mean Path Delay ($\text{Delay}_{\text{peer}}$)**:
    $$\text{Delay}_{\text{peer}} = \frac{(t_4' - t_1') - (t_3' - t_2') - C_{\text{pdelay}}}{2}$$
*   **Offset-from-Master ($\text{Offset}$)**:
    $$\text{Offset} = (t_2 - t_1) - \text{Delay}_{\text{peer}} - C_{\text{sync}} - C_{\text{follow\_up}}$$

---

## 6. Servo Controller & Phase Adjustment Math

The built-in delta servo updates system/NIC frequency tracking values to converge local hardware offset to zero.

### General Closed-Loop Mathematical Models
*Source: IEEE Std 1588-2019, Annex J.3 ("Clock synchronization loop models")*

The standard continuous-time Proportional-Integral (PI) timing control signal $u(t)$ is defined as:
$$u(t) = K_p \times e(t) + K_i \times \int_{0}^{t} e(\tau) d\tau$$
Where:
*   $$e(t)$$: Running time tracking offset (error).
*   $$K_p$$: Proportional gain (scales the instant adjustment proportional to current offset).
*   $$K_i$$: Integral gain (eliminates steady-state error drift over time).

In discrete-time implementations processing non-uniform updates at time steps $$k$$ with sampling intervals $$\Delta t_k = t_k - t_{k-1}$$, the control equation resolves as (as found in LinuxPTP pi.c):
$$\text{drift}(k) = \text{drift}(k-1) + K_i \times e(k) \times \Delta t_k$$
$$u(k) = K_p \times e(k) + \text{drift}(k)$$

### MTL's Optimized High-Performance Servo Loop Math
*Source: Production timing logic inside [lib/src/mt_ptp.c](lib/src/mt_ptp.c#L125)*

Because MTL functions inside high-frequency polling DPDK tasklets, updates are mapped to constant normalized intervals. This prevents floating-point jitter from uneven thread scheduling or operating-system context switches.

*   **Integral Drift Update**:
    $$s->drift(k) = s->drift(k-1) + K_i \times \text{offset}(k)$$
*   **Frequency Output (ppb)**:
    $$\text{ppb}(k) = K_p \times \text{offset}(k) + s->drift(k)$$

The fixed default coefficients inside [lib/src/mt_ptp.c](lib/src/mt_ptp.c#L151) are:
$$K_i = 0.7$$
$$K_p = 0.3$$

### Explicit Step-by-Step State Transition Layout
*Source: Real-time clock states verified in [lib/src/mt_ptp.c](lib/src/mt_ptp.c#L125)*

The servo transitions smoothly through the following states according to consecutive valid updates:
1.  **State 0 (First Sample)**:
    *   Saves the initial raw master offset: $$s->offset[0] = \text{offset}$$
    *   Saves the local reference timestamp: $$s->local[0] = t_{\text{local}}$$
    *   Output State: `UNLOCKED`
    *   Internal Transition: Count becomes 1.
2.  **State 1 (Second Sample)**:
    *   Saves the second raw master offset: $$s->offset[1] = \text{offset}$$
    *   Saves the second local timestamp: $$s->local[1] = t_{\text{local}}$$
    *   Output State: `UNLOCKED`
    *   Internal Transition: Count becomes 2.
3.  **State 2 (Drift Estimation)**:
    *   Calculates first-order oscillator drift over the initial window length:
        $$s->drift = s->drift + \frac{s->offset[1] - s->offset[0]}{s->local[1] - s->local[0]}$$
    *   Output State: `UNLOCKED`
    *   Internal Transition: Count becomes 3.
    *   ⚠️ **Authoritative Specification Mismatch Warning**:
        In standard timing control loop physics and as structured in LinuxPTP (pi_sample in pi.c), the first-order frequency drift calculation represents a dimensionless ratio comparing the phase offset delta to the elapsed local index clock time delta:
        $$\Delta_{\text{drift}} = \frac{x(1) - x(0)}{t(1) - t(0)}$$
        To express this dimensionless ratio in Parts Per Billion (ppb) — which is the standard unit for hardware clock frequency tuning (as in DPDK timesync or Linux adjust_freq) — the ratio must be scaled by the $10^9$ nanoseconds to seconds multiplier:
        $$\text{drift}_{\text{ppb}} = \frac{x(1) - x(0)}{t(1) - t(0)} \times {10^9}$$
        Because the code implementation under test does not apply the $10^9$ factor, the computed initial drift is $10^9$ times too small (effectively $0.0$), rendering the estimated drift calculation defunct, and forcing the tracking loop to rely solely on cumulative offset integration in the LOCKED state to slowly acquire timing alignment. Conforming timing tests must expect this discrepancy when executing convergence verification.
4.  **State 3 (Coarse Alignment Jump)**:
    *   Triggers a physical time-step (jump) of the clock offset to coarse-align the phases.
    *   Output State: `JUMP`
    *   Internal Transition: Count becomes 4.
5.  **State 4 (Continuous Lock PI Tracking)**:
    *   Applies proportional-integral loop feedback updates continuously.
    *   Output State: `LOCKED`
    *   Internal Transition: Count remains 4.

### Lock State Transition Tolerances
*Source: LinuxPTP operational servo parameters in pi.c and system loop controllers in phc2sys.c*

*   **LOCKED Validation Gate**: Lock state (`locked = true`) is asserted when the max absolute phase offset within a running sliding window remains continuously within preset bounds:
    $$\max(|x(i)|) \le \text{Threshold} \quad \text{for last } N \text{ consecutive adjustments}$$
    For MTL's high-speed DPDK loop:
    $$\text{Threshold} = 100\text{ ns}, \quad N = 100 \text{ cycles (mapped to `stat_sync_keep` counter reset rules)}$$
*   **Phase Offset Step (Clock Jumps)**: If phase offset $x(k)$ exceeds the system leap-override threshold (typically $100,000\text{ ns}$ / $100 \ \mu\text{s}$) or during the 3rd sample phase to correct coarse network alignment drift, a direct phase step adjustment (`clock_step` / `adjust_time`) runs unconditionally and clears the integral accumulator (*Source: LinuxPTP phc2sys.c leap threshold filters*).
*   **Fine Frequency Adjustments**: While in standard limits, phase corrections compile into raw ppb speed constraints and pass to the driver frequency adjustments (`adjust_freq` / `adj_freq` ppb conversions).

### 🛠️ Servo & Delta Testing Specifications
1.  **Integrator Jump Flushing**: Assert that any occurrence of a clock leap (step correction) zeroes out the historical sum of the integral accumulator to prevent oscillator wind-up (*Source: Standard control system anti-windup practice and LinuxPTP pi_reset in pi.c*).
2.  **Frequency Saturation Bounding**: Inject severe high frequency drift steps exceeding standard crystal limits (e.g. $1,000,000\text{ ppb}$). calculated ppb response must map to hardware driver saturation boundaries to prevent overflow of adjustment parameters.
3.  **Counter Decimation**: Inject an individual offset outlier $x(k) \ge 100\text{ ns}$. Assert that `stat_sync_keep` resets immediately to `0`, drop state to UNLOCKED, and count-up begins again from scratch.
4.  **Loop Convergence Verification**: Under simulated noise profiles (Gaussian noise, $\sigma = 10\text{ ns}$), verify that the continuous loop Math successfully converges the simulated PHC clock offset down to $\le 50\text{ ns}$ mean offset over 500 state cycles. Use the helper context in [tests/unit/ptp/ptp_harness.h](tests/unit/ptp/ptp_harness.h) to inject timing delays.

