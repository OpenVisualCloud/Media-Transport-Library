# 11 — Prior art: professional media I/O and streaming APIs

| | |
|---|---|
| **Topic** | How mature media I/O, streaming and async-I/O APIs handle scheduling, lateness reporting, buffering, clocks, ownership transfer and statistics. The goal is to extract patterns for the MTL unified API. |
| **Scope** | Blackmagic DeckLink, AJA NTV2 AutoCirculate, GStreamer, Linux io_uring, Vulkan (present timing, pNext), OpenXR frame loop, ALSA, JACK, PipeWire, CoreAudio, DPDK, NDI, SRT, WebRTC stats, OpenTelemetry metrics |
| **Date** | 2026-09-29 |
| **Author** | Research agent 11 (knowledge gathering only; nothing implemented) |
| **Related** | `00-pr1610-design-review.md` ("review §N"), especially §9 (ownership and completion), §11 (timing) and §14 (observable guarantees) |

**Evidence labels.** **[verified]** means read in a primary source during this research; the source is cited by short key and listed in [Sources](#sources). **[inferred]** means my reading or background knowledge, not checked against a primary source today. **[unknown]** means I could not determine it.

**Primary sources actually read.** DeckLink SDK Manual (March 2026 PDF, converted to text). `DeckLinkAPI.h` from SDK 12.0 and 12.2.2 (the copies vendored in OBS and GStreamer). AJA `ntv2publicinterface.h` and `ntv2card.h` (libajantv2 `main`). GStreamer design docs, the `gstbasesink` reference,
`gstbuffer.h` and `gstrtpjitterbuffer.c`. Linux `include/uapi/linux/io_uring.h` and man7 pages `io_uring(7)`, `io_uring_setup(2)`, `io_uring_enter(2)` and `io_uring_register(2)`. Vulkan-Headers `vulkan_core.h`, Vulkan-Docs `fundamentals.adoc` and docs.vulkan.org refpages. OpenXR 1.1 refpages. The
alsa-lib PCM doc and `include/pcm.h`. Kernel `Documentation/sound/designs/timestamping.rst` and `include/uapi/sound/asound.h`. JACK headers. PipeWire `stream.h`. CoreAudio SDK headers (a MacOSX11.3 SDK mirror). DPDK `rte_mbuf_dyn.h`, `rte_ethdev.h` and `rte_mbuf_core.h`. NDI SDK docs, SRT
`statistics.md`, W3C webrtc-stats, and the OpenTelemetry metrics data model.

**Not read.** The OpenXR full spec HTML (too large to fetch; I used refpages). The Vulkan queue-family-ownership and timeline-semaphore chapters (labelled [inferred] where used). The Apple online docs (I used SDK headers). The NDI SDK headers (I used web docs).

---

## 0. Summary

Five findings matter most for MTL:

1. **Every mature scheduled-output API reports a per-frame terminal result, and "late" is not the same as "dropped".** DeckLink's `BMDOutputFrameCompletionResult` is {Completed, DisplayedLate, Dropped, Flushed}. "Late" means the frame went out, but at a later slot than requested. "Dropped" means it
   never went out because a less-late frame superseded it. "Flushed" means a user action (stop or speed change) removed it **[verified: DeckLink §3.9]**. AJA has no per-frame result, only cumulative `acFramesDropped` and a polled "which cookie is on air" correlation **[verified: AJA]**. That is
   noticeably weaker.
2. **"Was I on time?" is best answered with requested time, actual time and margin, not a boolean.** `VkPastPresentationTimingGOOGLE` returns `desiredPresentTime`, `actualPresentTime`, `earliestPresentTime` and `presentMargin`, the last being how early the request was processed relative to the
   deadline **[verified: Vulkan refpage]**. `VK_EXT_present_timing` generalizes this to per-stage timestamps with an explicit time domain. A zero stage time means "unavailable", and `reportComplete` marks partial reports **[verified: vulkan_core.h, refpage]**.
3. **Result queues that cannot lose results are built by admission control, not by overflow buffers.** Vulkan reserves a results-queue slot when you present. If there is none, the present fails with `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT` **[verified]**. DeckLink `ScheduleVideoFrame` returns
   `E_OUTOFMEMORY`, "Too many frames are already scheduled" **[verified]**. io_uring `IORING_FEAT_NODROP` instead stashes overflow CQEs in kernel memory. It can still drop under OOM (`-EBADR`) **[verified: io_uring_setup(2), io_uring_enter(2)]**. MTL's data plane cannot allocate, so it should copy
   the admission model.
4. **Timestamps are multi-clock, validity-flagged and taken as a coherent snapshot.** CoreAudio's `AudioTimeStamp` carries sample time, host time, rate scalar, word clock and SMPTE, plus `mFlags` validity bits **[verified]**. ALSA reports the requested timestamp type, the *actual* type used, and an
   accuracy with its own validity bit. It reports exactly one (system, audio) pair "to avoid any interpretation issues" **[verified: timestamping.rst, pcm.h]**. AJA's `FRAME_STAMP` pairs host-OS time with the device 48 kHz audio clock at the same VBI **[verified]**.
5. **Stats taxonomies converge on monotonic cumulative counters plus separate gauges, with rates and averages computed by the reader.** WebRTC says outright that having the implementation compute "average" rates "is not a good idea". It says to report sums and counts instead **[verified: W3C
   webrtc-stats]**. OpenTelemetry formalizes cumulative vs delta temporality with a start timestamp for reset detection **[verified]**. SRT and ALSA show the costs of reset-on-read and moving-average alternatives **[verified]**.

---

## 1. Blackmagic DeckLink SDK

Sources: the DeckLink SDK Manual dated March 2026 (sections given inline), and `DeckLinkAPI.h` from SDK 12.0/12.2.2.

### 1.1 Scheduling model

| Call | Semantics |
|---|---|
| `ScheduleVideoFrame(frame, displayTime, displayDuration, timeScale)` | "schedule a frame for asynchronous playback at a specified time". Frames "may be scheduled before calling StartScheduledPlayback to preroll". Returns `E_OUTOFMEMORY` "Too many frames are already scheduled" **[verified: §2.5.3.13]** |
| `StartScheduledPlayback(playbackStartTime, timeScale, playbackSpeed)` | "Scheduled playback starts immediately when StartScheduledPlayback is called, setting the current scheduler time to the playbackStartTime parameter. Scheduled frames are output as the current scheduler time reaches the scheduled frame's display time." **[verified: §2.5.3.25]** |
| `StopScheduledPlayback(stopPlaybackAtTime, *actualStopTime, timeScale)` | Stops "immediately or at a specified time. Any frames or audio scheduled after the stop time will be flushed". Returns the actual stop time **[verified: §2.5.3.26]** |
| `GetScheduledStreamTime(timeScale, *streamTime, *speed)` | "elapsed time since scheduled playback began" **[verified: §2.5.3.27]** |
| `GetBufferedVideoFrameCount(*count)` | "gets the number of frames queued" (a polled gauge) **[verified: §2.5.3.15]** |
| `GetHardwareReferenceClock(timeScale, *hwTime, *timeInFrame, *ticksPerFrame)` | "a clock that is locked to the rate at which the DeckLink hardware is outputting frames. The absolute values returned by this method are meaningless", only differences are useful **[verified: §2.5.3.29]** |
| `GetFrameCompletionReferenceTimestamp(frame, timeScale, *ts)` | "the time that the frame has been output ... locked to the system clock". It is **valid only** "if this method is called within the ScheduledFrameCompleted callback and if the frame ... has not been re-scheduled" **[verified: §2.5.3.30]** |
| `GetReferenceStatus` | Genlock status bitmask (`bmdReferenceLocked`, `bmdReferenceNotSupportedByHardware`) **[verified: header]** |

The clock model is a stream timeline in caller-chosen rational units (`BMDTimeValue` over `BMDTimeScale`). It is anchored to "now" by `StartScheduledPlayback`, with a separate free-running hardware clock and a system-clock completion timestamp **[verified]**. There is no absolute wall or TAI time in
the scheduling call itself **[verified: signatures]**. That fits SDI, where the output is locked to the genlock reference. It is not enough for ST 2110, where TAI epoch alignment is part of the wire contract **[inferred]**.

### 1.2 Completion result: exact semantics

`IDeckLinkVideoOutputCallback::ScheduledFrameCompleted(IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult result)` is "called for each frame as its processing is completed". "If the application is managing its own frame buffers, they should be disposed or reused inside the ScheduledFrameCompleted callback" **[verified: §2.5.6]**.

From §3.9 "Output Frame Completion Results Flags", verbatim:

> Frames are "flushed" when they have previously been scheduled but are no longer needed due to an action initiated by the API user, e.g. stopped playback or a changed playback speed or direction.
> If frame scheduling falls behind frame output, the hardware will output the least late frame available. When this happens, the frame will receive a completion status of "displayed late".
> Frames that are never displayed due to a less late frame being available will receive a completion status of "dropped".

| Result | Frame reached output? | Cause |
|---|---|---|
| `bmdOutputFrameCompleted` | yes, on time | normal |
| `bmdOutputFrameDisplayedLate` | yes, after its scheduled time | scheduling fell behind; this was the least-late frame available |
| `bmdOutputFrameDropped` | no | superseded by a less-late frame |
| `bmdOutputFrameFlushed` | no | user action: stop, or speed/direction change |

Observations:

- Exactly one terminal result per scheduled frame, and it is also the buffer-reuse signal **[verified: §2.5.6 text]**.
- There is no "failed/error" result, and no reason code beyond the four values **[verified: enum]**.
- What goes out on a slot with no frame (repeat last, or black) is not stated in the text I read **[unknown]**.
- Callback discipline: callbacks run "on a dedicated callback thread". The application must "ensure that any application processing on the callback thread takes less time than a frame time" or frames are "dropped or delayed" **[verified: §2.5.6.1]**. `ScheduledPlaybackHasStopped` is a separate lifecycle event **[verified]**.

### 1.3 Audio

- `ScheduleAudioSamples(buffer, sampleFrameCount, streamTime, timeScale, *sampleFramesWritten)` accepts **partially**: the out-parameter is the "Actual number of sample frames scheduled". With `bmdAudioOutputStreamContinuous`, `streamTime = timeScale = 0` appends after the currently buffered samples. `bmdAudioOutputStreamTimestamped` requires timestamps **[verified: §2.5.3.21, header]**.
- `GetBufferedAudioSampleFrameCount` is an audio buffer-level gauge in sample frames. `FlushBufferedAudioSamples` discards what is queued **[verified: §2.5.3.22–23]**.
- Audio preroll is **pull-based**: `BeginAudioPreroll` makes the driver call `RenderAudioSamples(preroll=true)`, "at a rate of 50Hz during playback". The app schedules audio in that callback until `EndAudioPreroll` **[verified: §2.5.3.19–20, RenderAudioSamples text]**.

### 1.4 Memory and ownership

- The current SDK has `IDeckLinkVideoBuffer` with `GetBytes`, `GetSize`, `StartAccess(BMDBufferAccessFlags)` and `EndAccess(flags)`. `GetBytes` returns `E_ACCESSDENIED` unless `StartAccess` was called first. Calls must be balanced per access flag, and "The final release of this interface should
  resolve all outstanding calls to EndAccess" **[verified: §2.5.53]**. This is an explicit CPU-access lease with declared intent (read/write), which is very close to the review's "access lease" (§7, §9).
- The current SDK also has `IDeckLinkVideoBufferAllocatorProvider` and `IDeckLinkVideoBufferAllocator::AllocateVideoBuffer`, plus `CreateVideoFrameWithBuffer(..., IDeckLinkVideoBuffer*, ...)` for app-supplied buffers **[verified: §2.5.3.9, §2.5.54–55]**. SDK 12.x had the simpler
  `IDeckLinkMemoryAllocator::AllocateBuffer/ReleaseBuffer` and `SetVideoOutputFrameMemoryAllocator` **[verified: SDK 12 header]**. In which SDK version the change happened is **[unknown]**.
- Frames are reference-counted COM objects. The completion callback hands the frame back, and the app reuses or releases it **[verified]**.

### 1.5 Input

- `VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*)`. The frame "is only valid for the duration of the callback". To keep it, call `AddRef` **[verified: §2.5.10.1]**. The video frame can be NULL when only audio arrived ("If video processing is not fast enough, audio will still be delivered") **[verified]**.
- Each input frame has `GetStreamTime(&frameTime, &frameDuration, timeScale)` and `GetHardwareReferenceTimestamp(timeScale, &frameTime, &frameDuration)` **[verified: §2.5.11]**.
- Signal loss is a **per-frame flag**, not a missing frame: `bmdFrameHasNoInputSource` means "No input source was detected – frame is invalid" **[verified: §3.6 Frame Flags, header]**. Cadence is preserved and validity is flagged.
- Format detection is opt-in (`bmdVideoInputEnableFormatDetection`) and capability-gated (`BMDDeckLinkSupportsInputFormatDetection`). The callback `VideoInputFormatChanged(BMDVideoInputFormatChangedEvents bitmask, IDeckLinkDisplayMode* newMode, BMDDetectedVideoInputFormatFlags)` reports *which*
  properties changed (display mode, field dominance, colorspace) and the new values **[verified: §2.5.10.2, header]**.

### 1.6 ST 2110 and statistics (DeckLink IP)

- DeckLink IP cards support SMPTE 2110. The **media API is unchanged** (`IDeckLinkOutput`/`IDeckLinkInput`). IP flows are a separate object family (`IDeckLinkIPExtensions`, `IDeckLinkIPFlow::Enable/Disable`, attributes, status, settings). The SDP is read through
  `IDeckLinkIPFlowStatus::GetString(bmdDeckLinkIPFlowSDP)`. "The same functionality can be achieved with an off-the-shelf NMOS controller" **[verified: §2.4.15, §2.5.59]**.
- `IDeckLinkStatistics::GetInt(statID, &value)`, `GetIntWithParam(statID, param, &value)` and `GetStringWithParam` are **key-addressed**, extensible statistics. Examples: PTP loss-of-lock counter, PTP DPLL error margin, device temperature, and per-Ethernet-port `EthernetRxPackets` and `EthernetRxDroppedPackets` **[verified: §2.5.63, BMDDeckLinkStatisticID]**.
- Synchronized multi-output start: a "playback group" configuration, `bmdVideoOutputSynchronizeToPlaybackGroup`, and `StartScheduledPlayback` on any member starts all of them **[verified: §2.4.13 Synchronized Capture/Playback]**.

---

## 2. AJA NTV2 AutoCirculate

Sources: `ntv2publicinterface.h` and `ntv2card.h` (libajantv2 `main`).

### 2.1 Model

AutoCirculate is a driver-managed **ring of on-device frame buffers**. `AutoCirculateInitForOutput(channel, inFrameCount = 7, audioSystem, optionFlags, numChannels, startFrame, endFrame)` reserves the ring. The header's own warning: "Fewer frames reduces latency, but increases the likelihood of
frame drops" **[verified: ntv2card.h]**. The host copies into the ring with `AutoCirculateTransfer`, which "will block until the transfer completes (or fails)". Before calling it for playout, the app is expected to check `AUTOCIRCULATE_STATUS::CanAcceptMoreOutputFrames` **[verified]**.

State machine, `NTV2AutoCirculateState` **[verified]**: `DISABLED`, `INIT`, `STARTING`, `PAUSED`, `STOPPING`, `RUNNING`, `STARTING_AT_TIME`. `AutoCirculateStart(channel, inStartTime = 0)` can defer the start until "the host OS tick clock exceeds the inStartTime value" **[verified]**.

Control verbs **[verified]**:

- `AutoCirculateStop(channel, inAbort)`: graceful stop at the next VBI, or immediate abort.
- `AutoCirculatePause`/`Resume(channel, inClearDropCount)`.
- `AutoCirculateFlush(channel, inClearDropCount)`: "On playout, all queued frames that have already been transferred to the device (that haven't yet played) are discarded". It has "no effect on the Active Frame". The state is unchanged.
- `AutoCirculatePreRoll(channel, n)`: only for frames DMA'd outside AutoCirculate.

### 2.2 Status and per-transfer results

`AUTOCIRCULATE_STATUS` (polled), with every field **[verified]**:

- `acState`, `acStartFrame`, `acEndFrame`, `acActiveFrame`.
- `acRDTSCStartTime` and `acAudioClockStartTime`: the first VBI after start, in host clock and in device 48 kHz audio clock.
- `acRDTSCCurrentTime` and `acAudioClockCurrentTime`: the same two clocks sampled at the status call.
- `acFramesProcessed`, `acFramesDropped` (cumulative since start), `acBufferLevel` ("Number of buffered frames in driver ready to capture or play"), `acOptionFlags`, `acAudioSystem`.

`AUTOCIRCULATE_TRANSFER_STATUS` (returned in place by each transfer) **[verified]**:

- `acState` after the transfer.
- `acTransferFrame` (-1 if failed).
- `acBufferLevel`, `acFramesProcessed`, `acFramesDropped`.
- `acFrameStamp` (a `FRAME_STAMP`), plus audio and anc byte counts.

`FRAME_STAMP` is the timing record **[verified]**:

- Capture only:
  - `acFrameTime`: the VBI timestamp when the frame started recording, on the hi-res OS clock.
  - `acAudioClockTimeStamp`: the same instant on the device 48 kHz audio clock.
  - `acAudioInStartAddress` and `acAudioInStopAddress`.
- `acStartSample`: "may be used to check sync ... if the clocks drift or the user supplies unaligned audio sizes, then this will give the current difference from expected versus actual position".
- `acCurrentTime` and `acAudioClockCurrentTime`: both clocks sampled when the driver handled the call.
- `acCurrentFrameTime`: the VBI of the frame "currently going out the SDI connector".
- `acCurrentReps`: repeats remaining on playout, drops on record.
- `acCurrentUserCookie`: "The frame's AUTOCIRCULATE_TRANSFER::acInUserCookie value ... This can tell clients which frame was going 'on-the-air' (i.e. 'out the jack') at the last VBI."

`AUTOCIRCULATE_TRANSFER` inputs **[verified]**:

- The client owns the host video, audio and anc buffers ("AJA recommends ... page-aligned").
- `acOutputTimeCodes`, `acInUserCookie` (64-bit), `acFrameRepeatCount`, `acDesiredFrame`.
- Per-transfer format changes are allowed only when init used `AUTOCIRCULATE_WITH_FBFCHANGE`, and so on.

What AJA does **not** have: a per-frame terminal result saying "this frame was late or dropped". Drops are an aggregate counter. On-air identification is by polling the cookie of the frame on air at the last VBI **[verified by absence in the structs above; inferred that apps diff counters]**.

### 2.3 Memory

`DMABufferLock(NTV2Buffer, inMap, inRDMA)` "Page-locks the given host buffer to reduce transfer time and CPU usage of DMA transfers". It can also lock the segment map, or a GPUDirect buffer for P2P. There are matching `DMABufferUnlock`/`UnlockAll` calls, and `DMABufferAutoLock` **[verified]**. This
is the analogue of MTL memory registration: explicit, outside the per-frame call, and optional (unlocked buffers still work, only slower) **[verified "reduce transfer time"; inferred optionality from API shape]**.

### 2.4 Struct evolution: `NTV2_HEADER` and `NTV2_TRAILER`

Every new NTV2 struct starts with `NTV2_HEADER` **[verified]**:

- `fHeaderTag` (a FourCC, which also detects endianness) and `fType` (a FourCC struct type).
- `fHeaderVersion` and `fVersion` (struct body version).
- `fSizeInBytes` ("total size ... including header, body and trailer") and `fPointerSize`.
- `fOperation` (an RPC connection ID since SDK 16.3) and `fResultStatus` ("set by driver", SDK 17.5).

It ends with `NTV2_TRAILER` (`fTrailerVersion`, the SDK version, and `fTrailerTag`). This is a heavier form of `struct_size`: a type tag, a size, a version, a trailer canary, and a per-struct result code.

A cautionary note sits in AJA's own header on `AutoCirculateTransfer` for S2110 firmware: "@bug ... this method performs many heap allocations in order to transparently support normal (VPID and RP188) and custom ancillary data. This feature should be re-implemented to use separate, per-channel, private, pre-allocated heaps" **[verified]**.

---

## 3. GStreamer

### 3.1 Clock model **[verified: design/synchronisation]**

- `B.running_time = (B.timestamp - (S.start + S.offset)) / ABS(S.rate) + S.base`. On the clock side, `C.running_time = absolute_time - base_time`.
- The sink waits until `B.sync_time = B.running_time + base_time`. Stream time (position) "is never used for synchronisation against the clock".
- The pipeline clock can be a PTP clock (`GstPtpClock`) **[inferred; not fetched]**.

### 3.2 Latency **[verified: design/latency]**

- The LATENCY query carries `live` (bool), `min-latency` and `max-latency` (`NONE` means infinite).
  - `min_latency = upstream_min + own_min`, where each element reports the longest time it will hold data back.
  - Max: blocking elements add their buffer size. Leaky elements take `MIN(upstream_max, own_max)`.
- The pipeline picks `MAX(all min)` and distributes it in a LATENCY event. Sinks add it to their sync times.
- If `MIN(all max) < chosen latency`, "we have an impossible situation and we must generate an error indicating that this pipeline cannot be played".
- Live sources: without latency compensation, "all buffers will be dropped" because a live source's buffer with timestamp T exists only at T+D.
- Dynamic latency changes are posted on the bus and cause glitches, "reserved for special conditions".

### 3.3 Lateness and QoS **[verified: design/qos, GstBaseSink]**

- Jitter `J = CT - B`: negative means early (the sink waits), positive means late.
- The QoS event (upstream) carries `type` (OVERFLOW, UNDERFLOW or THROTTLE), `proportion`, `jitter` (the name for diff) and `timestamp` (running time).
- The QoS *message* (bus) carries `live`, `running-time`, `stream-time`, `timestamp`, `duration`, `jitter`, `proportion`, `quality`, `format`, and `processed`/`dropped` counts "since the last READY state change or flush".
- `GstBaseSink` properties:
  - `max-lateness`: "The maximum time in nanoseconds that a buffer can be late before it is dropped", with -1 meaning unlimited. The basesink default is -1. Video sinks override it (commonly 20 ms) **[inferred for the override value]**.
  - `sync`, `qos`, and `render-delay` ("additional delay between synchronisation and actual rendering", added to latency).
  - `ts-offset`, `throttle-time`, and `processing-deadline` (since 1.16: "Maximum amount of time ... that the pipeline can take for processing the buffer. This is added to the latency of live pipelines").
- `stats` (since 1.18) is `application/x-gst-base-sink-stats` {`average-rate`, `dropped`, `rendered`}.
- The lateness rule: the buffer is late when presentation time plus duration is before the clock's current time. "If the frame is later than max-lateness, the sink will drop the buffer without calling the render method".

### 3.4 Buffer pools **[verified: design/bufferpool]**

- Upstream initiates the ALLOCATION query. Downstream answers with pools `{pool, size, min_buffers, max_buffers}`, allocators `{allocator, params (align/padding)}`, and accepted metas.
- On an exhausted pool, acquire blocks until a buffer returns. With `GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT` it returns `GST_FLOW_EOS`. On an inactive pool it returns `GST_FLOW_FLUSHING` immediately, and deactivating the pool wakes blocked acquires with FLUSHING.
- Buffers return to the pool when their refcount reaches 0. A small `max_buffers` rate-limits the producer ("rate limited by the rate at which buffers are recycled").

### 3.5 Per-buffer metadata and reference timestamps **[verified: gstbuffer.h, gstrtpjitterbuffer.c]**

- `GstReferenceTimestampMeta { GstCaps* reference; GstClockTime timestamp, duration; GstStructure* info /*1.28*/ }`. The clock identity lives in caps, for example `timestamp/x-ptp, version=IEEE1588-2008, domain=1`, `timestamp/x-ntp, host=..., port=123`, `timestamp/x-unix`, `timestamp/x-system-monotonic`. A buffer "can contain multiple" of these metas.
- `rtpjitterbuffer`:
  - `add-reference-timestamp-meta` attaches the sender's reconstructed reference-clock timestamp (RFC 7273). `rfc7273-reference-timestamp-meta-only` keeps it as meta without affecting PTS.
  - Stats {`num-pushed`, `num-lost`, `num-late`, `num-duplicates`, `avg-jitter`, rtx...}.
  - `post-drop-messages` posts coalesced "drop-msg" structures carrying `seqnum`, `timestamp`, `reason`, `num-too-late` and `num-drop-on-latency` "since last drop message", rate-limited by `drop-messages-interval`.
  - `drop-on-latency` drops the oldest packets when full.

---

## 4. Linux io_uring

Sources: `include/uapi/linux/io_uring.h` (torvalds `master`) and the man7 pages.

### 4.1 Identity and cardinality

- `user_data` "is passed unchanged from submission to completion ... uniquely identifying submissions" **[verified: io_uring(7)]**.
- "The kernel places exactly one matching CQE in the CQ for every SQE you submit" **[verified: io_uring(7)]**. There are two documented exceptions:
  - Multishot requests set `IORING_CQE_F_MORE` ("parent SQE will generate more CQE entries").
  - Zero-copy send posts a second CQE flagged `IORING_CQE_F_NOTIF` when the kernel is done with the buffer. With `IORING_SEND_ZC_REPORT_USAGE`, it reports whether data was actually copied (`IORING_NOTIF_USAGE_ZC_COPIED`) **[verified: io_uring.h]**. This splits "operation result" from "buffer reusable", which is exactly the TX distinction MTL faces with packet references held in the NIC ring.
- "Requests ... can complete in any order"; ordering needs `IOSQE_IO_LINK` **[verified: io_uring(7)]**.

### 4.2 CQ overflow: how "no lost completions" is (almost) guaranteed

- With `IORING_FEAT_NODROP` (5.5+): "A dropped event can only occur if the kernel runs out of memory". When the CQ ring is full, "the kernel stores the event internally until such a time that the CQ ring has room". Older kernels failed submission with `-EBUSY` if overflow could not be flushed. If
  overflow storage cannot be allocated, "it will be visible by an increase in the overflow value on the cqring" (`io_cqring_offsets.overflow`). Since 5.19, the next wait returns `-EBADR` **[verified: io_uring_setup(2), io_uring_enter(2)]**.
- `IORING_SQ_CQ_OVERFLOW` in the SQ ring flags signals the overflow state to userspace **[verified: io_uring.h]**.
- `IORING_SETUP_CQSIZE` lets the application size the CQ (`> entries`, rounded to a power of two) **[verified]**. The default of 2x SQ entries is **[inferred]**; the man page does not state it.
- Lesson: NODROP is "no drop unless OOM, and then tell you". The guarantee relies on allocation, which MTL's data plane forbids.

### 4.3 Polling thread and wakeup (the tasklet analogue)

`IORING_SETUP_SQPOLL` creates a kernel thread that polls the SQ. "If the kernel thread is idle for more than sq_thread_idle milliseconds, it will set the IORING_SQ_NEED_WAKEUP bit". The application must then call `io_uring_enter(fd, 0, 0, IORING_ENTER_SQ_WAKEUP)` **[verified: io_uring_setup(2)]**.
This makes "poller asleep" a flag the producer can see and a cheap, explicit wake protocol. It maps onto MTL tasklets that sleep when idle.

### 4.4 Waiting, timeouts, cancellation, links

- `IORING_ENTER_GETEVENTS` waits for `min_complete`. `IORING_ENTER_EXT_ARG` with `struct io_uring_getevents_arg { sigmask; sigmask_sz; min_wait_usec; ts; }` adds a timeout and a minimum batching wait **[verified: io_uring.h, io_uring_enter(2)]**.
- `IORING_OP_ASYNC_CANCEL` returns 0 if found, `-ENOENT` if not found, or `-EALREADY` if "attempted canceled ... may or may not terminate" **[verified]**. The cancelled request itself completes with `-ECANCELED` **[inferred]**; that page only shows `-ECANCELED` for broken link chains and timeouts.
- Linked chains: when a link fails, "the remaining unstarted part of the chain will be terminated and completed with -ECANCELED". `IOSQE_IO_HARDLINK` does not sever. `IORING_OP_LINK_TIMEOUT` completes with `-ETIME` if it fired, or `-ECANCELED` if the linked op won **[verified]**. Cancellation still produces exactly one CQE per SQE.

### 4.5 Registered and provided buffers

- `IORING_REGISTER_BUFFERS`: the pages "will be locked in memory", "charged against the user's RLIMIT_MEMLOCK", with a 1 GiB-per-buffer limit. Use them with `READ_FIXED`/`WRITE_FIXED` and `buf_index` **[verified: io_uring_register(2)]**.
- **Resource tags** (`REGISTER_BUFFERS2`, `BUFFERS_UPDATE`): "after the resource had been unregistered and it's not used anymore ... a CQE will be posted with user_data set to the specified tag" **[verified]**. This is a *deferred release notification*, an alternative to returning `-EBUSY` from unregister.
- `IORING_REGISTER_PBUF_RING`: provided-buffer ring for receives, where the kernel picks a buffer and the CQE has `IORING_CQE_F_BUFFER` with the buffer ID. Ring entries are a power of two, maximum 32768 **[verified]**. The behavior when the ring is empty (`-ENOBUFS`) is **[inferred]**; it is not on the page I read.

---

## 5. Vulkan and OpenXR

### 5.1 Struct-chain evolution **[verified: Vulkan-Docs fundamentals.adoc, vulkan_core.h]**

- Every extensible struct begins `{ VkStructureType sType; const void* pNext; }`. Base types `VkBaseInStructure`/`VkBaseOutStructure` exist for walking chains.
- The rule, verbatim: "Any component of the implementation (the loader, any enabled layers, and drivers) must skip over, without processing (other than reading the sType and pNext members) any extending structures in the chain not defined by core versions or extensions supported by that component."
- Output structs (for example `VkPastPresentationTimingEXT`) also carry sType/pNext, so *results* can be extended too. Array outputs use the two-call count/fill idiom.

### 5.2 Present timing: "was my frame on time?"

`VK_GOOGLE_display_timing` **[verified: vulkan_core.h, refpage]**:

- The request is `VkPresentTimeGOOGLE { presentID; desiredPresentTime; }`, with non-zero meaning "not ... presented any sooner than desiredPresentTime".
- `vkGetPastPresentationTimingGOOGLE` returns `{ presentID, desiredPresentTime, actualPresentTime, earliestPresentTime, presentMargin }`:
  - `earliestPresentTime` is "the time when the image ... could have been displayed".
  - `presentMargin` is "how early the vkQueuePresentKHR command was processed compared to how soon it needed to be processed" and still be shown at `earliestPresentTime`.
- "The results for a given swapchain and presentID are only returned once". Retention depth is **[unknown]**.

`VK_EXT_present_timing` (spec version 3) **[verified: vulkan_core.h, refpages]**:

- The request is `VkPresentTimingInfoEXT { flags; targetTime; timeDomainId; presentStageQueries; targetTimeDomainPresentStage; }`.
  - Flags: `PRESENT_AT_RELATIVE_TIME`, and `PRESENT_AT_NEAREST_REFRESH_CYCLE`. Without the latter, "the application would strictly prefer the image to not be visible before targetTime".
  - The spec says the timing features "do not provide a strict guarantee".
- Present stages: `QUEUE_OPERATIONS_END`, `REQUEST_DEQUEUED`, `IMAGE_FIRST_PIXEL_OUT`, `IMAGE_FIRST_PIXEL_VISIBLE`. The result gives a `VkPresentStageTimeEXT {stage, time}` per stage, where "a time value of 0, indicating that results for that present stage are not available".
- Results: `VkPastPresentationTimingEXT { presentId, targetTime, presentStageCount, pPresentStages, timeDomain, timeDomainId, reportComplete }`. Query flags `ALLOW_PARTIAL_RESULTS` and `ALLOW_OUT_OF_ORDER_RESULTS`.
- Time domains (`VkTimeDomainKHR`): `DEVICE`, `CLOCK_MONOTONIC`, `CLOCK_MONOTONIC_RAW`, `QUERY_PERFORMANCE_COUNTER`, `PRESENT_STAGE_LOCAL_EXT`, `SWAPCHAIN_LOCAL_EXT`. The implementation may substitute a fallback domain and report which one it used.
- `timingPropertiesCounter` and `timeDomainsCounter` let the app detect that refresh or time-domain properties changed.
- **Bounded results queue with admission control.** `vkSetSwapchainPresentTimingQueueSizeEXT(size)` allocates "the swapchain-internal timing results queue". When `presentStageQueries != 0`, a slot is reserved at present time. "If no slot is free ... presentation fails and returns VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT". Shrinking below the pending count returns `VK_NOT_READY`.
- `VK_KHR_present_wait2`: `vkWaitForPresent2KHR(device, swapchain, {presentId, timeout})` blocks until a given present has happened. Paired with `VK_KHR_present_id2` (64-bit IDs).

### 5.3 Ownership transfer (queue-family acquire/release) **[inferred; spec chapter not fetched this session]**

Vulkan transfers exclusive resource ownership between queue families with a matched *release* barrier on the source queue and *acquire* barrier on the destination, ordered by a semaphore. Timeline semaphores give a 64-bit monotonically increasing payload that host and device can wait on or signal.
Mapping to MTL: submit is a release to MTL, a terminal result is a release back, and a timeline value is a cheap "all submissions ≤ N are terminal" watermark.

### 5.4 OpenXR frame loop **[verified: OpenXR 1.1 refpages]**

- `xrWaitFrame` "throttles the application frame loop in order to synchronize application frame submissions with the display". It returns `XrFrameState { predictedDisplayTime, predictedDisplayPeriod, shouldRender }`, and `predictedDisplayTime` "must be monotonically increasing".
- `xrBeginFrame` without a preceding successful wait returns `XR_ERROR_CALL_ORDER_INVALID`. A second begin without an end returns the *success* code `XR_FRAME_DISCARDED`. Runtimes "must not perform frame synchronization or throttling through the xrBeginFrame function".
- `shouldRender = false` tells the app to skip GPU work but keep the call cadence.
- Pattern: the runtime tells the producer *which* display time the next frame is for, before the producer renders.

---

## 6. Audio APIs

### 6.1 ALSA

- Xrun: the state `SND_PCM_STATE_XRUN` means "The PCM device reached overrun (capture) or underrun (playback)". I/O returns `-EPIPE`. Recover with `snd_pcm_recover()`, or with `prepare`/`drop`/`drain` **[verified: alsa-lib pcm doc]**.
- `avail` is how much can be written. `delay` is "the time it will take to hear a new sample after all queued samples have been played out". `snd_pcm_avail_delay()` returns both "in sync" **[verified]**.
- `struct snd_pcm_status` has `state`, `trigger_tstamp` ("time when stream was started/stopped/paused"), `tstamp`, `appl_ptr`, `hw_ptr`, `delay`, `avail`, `avail_max` ("max frames available on hw since last status"), `overrange`, `audio_tstamp` ("sample counter, wall clock, PHC or on-demand
  sync'ed"), `driver_tstamp`, and `audio_tstamp_accuracy` **[verified: asound.h]**. `avail_max` and `overrange` "are reset to zero after the status call" **[verified]**.
- Audio timestamp types: `DEFAULT` (DMA/hw_ptr), `LINK`, `LINK_ABSOLUTE`, `LINK_ESTIMATED`, `LINK_SYNCHRONIZED` **[verified: asound.h]**.
  - The config is `{type_requested, report_delay}`. The report is `{valid, actual_type ("actual type if hardware could not support requested timestamp"), accuracy_report ("0 if accuracy unknown"), accuracy (ns)}` **[verified: alsa-lib pcm.h]**.
  - "In case the application requests an audio tstamp that is not supported ... the type is overridden as DEFAULT" and the override is reported **[verified: timestamping.rst]**.
- Only a single (system, audio) timestamp pair is reported: "a conscious design decision ... the more timestamps are read the more imprecise the combined measurements are". `driver_tstamp` exists because the system timestamp may be latched with a delay relative to avail/delay **[verified: timestamping.rst]**.
- The timestamping doc's latency ladder (analog, link, DMA, app, full buffer) is a clean model of the separate delays behind "delay" **[verified]**.

### 6.2 JACK

- `jack_set_xrun_callback(client, JackXRunCallback, arg)` fires on every xrun, **with no information in the callback**. The magnitude is fetched separately via `jack_get_xrun_delayed_usecs()`. There is also `jack_get_max_delayed_usecs()` and `jack_reset_max_delayed_usecs()` **[verified: jack.h, statistics.h]**.
- `jack_get_cycle_times(client, &current_frames, &current_usecs, &next_usecs, &period_usecs)`:
  - "Unless there was an xrun, skipped cycles, or the current cycle is the first ... current_usecs will always be the value of next_usecs of the previous cycle". So discontinuity is detectable from the timeline.
  - `period_usecs` is a DLL estimate, not exactly `next - current`.
  - **[verified: jack.h]**
- Latency is reported as a range: `jack_latency_range_t { min; max; }` in frames, per port and per direction (capture or playback), recomputed via a latency callback **[verified: types.h]**.

### 6.3 PipeWire

- `struct pw_time` is a snapshot for extrapolation **[verified: stream.h]**:
  - `now` (ns when updated) and `rate` (the fraction of ticks).
  - `ticks`: monotonic graph-driver time; it "can be used ... to detect discontinuities in the timeline caused by xruns".
  - `delay`: to the device edge; "can be negative".
  - `queued` (sum of app-set `pw_buffer.size`, in app-chosen units), `buffered` (in the resampler), `queued_buffers`, `avail_buffers`, and `size`.
- The documentation gives the formula for total latency of a newly queued buffer as buffered + queued + (delay − elapsed) **[verified]**. The time vocabulary is explicit and additive.
- ABI evolution: `pw_stream_get_time_n(stream, struct pw_time*, size_t size)` is a size-parameterized getter, with fields "Since 0.3.50" and "Since 1.1.0". Event tables start with `uint32_t version` (`PW_VERSION_STREAM_EVENTS 2`) **[verified]**.
- `pw_buffer.user_data` is "returned unmodified each time a buffer is dequeued". `requested` is a playback hint, and `time` is the capture cycle time **[verified]**.

### 6.4 CoreAudio

- `AudioTimeStamp { Float64 mSampleTime; UInt64 mHostTime; Float64 mRateScalar; UInt64 mWordClockTime; SMPTETime mSMPTETime; AudioTimeStampFlags mFlags; UInt32 mReserved; }` is "different representations of the same point in time". The flags are `kAudioTimeStampSampleTimeValid`, `HostTimeValid`,
  `RateScalarValid`, `WordClockTimeValid`, `SMPTETimeValid` and `SampleHostTimeValid`. `mRateScalar` is "the ratio of actual host ticks per sample frame to the nominal" **[verified: CoreAudioBaseTypes.h]**.
- `AudioDeviceIOProc(device, inNow, inInputData, inInputTime, outOutputData, inOutputTime, clientData)` **[verified: AudioHardware.h]**:
  - `inNow` is the cycle start, "includes any scheduling latency".
  - `inInputTime` is when the first input frame "was acquired".
  - `inOutputTime` is when the first output frame "will be passed to the hardware". This is the deadline, given to the producer ahead of time.
- `kAudioDeviceProcessorOverload` exists "so that clients can be notified when the AudioDevice detects that an IO cycle has run past its deadline", and it is dispatched synchronously from the IO thread **[verified]**. `kAudioDevicePropertySafetyOffset` is the safe distance from the hardware
  position, and `kAudioDevicePropertyLatency` is the device latency **[verified: names; exact latency semantics inferred]**.

---

## 7. DPDK (MTL's substrate)

- `rte_eth_tx_burst` "returns the number of packets it actually sent", which is partial acceptance. It "transparently free[s] the memory buffers of packets previously sent" when free descriptors drop below `tx_free_thresh` **[verified: rte_ethdev.h]**. So mbuf release, and with it an extbuf
  `free_cb(addr, opaque)` at refcnt 0 (`struct rte_mbuf_ext_shared_info { free_cb; fcb_opaque; refcnt; }`), is lazy and batched. It is **not** a wire-time signal. `rte_eth_tx_done_cleanup(port, queue, free_cnt)` forces reclaim **[verified]**.
- Launch time: `RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP` with the `rte_dynfield_timestamp` field and the `rte_dynflag_tx_timestamp` flag. Units and reference "are not explicitly defined but are maintained always the same for a given port". Crucially: "If the specified one is in the past it should be
  ignored, if one is in the distant future it should be capped". There is no reordering, and "The timestamps might be put only in the first packet in the burst" **[verified: rte_mbuf_dyn.h]**. So a late launch time is sent *immediately and silently*. The NIC reports no lateness, and MTL must detect
  it before handing packets to the PMD.
- `rte_eth_read_clock(port, &clock)` returns raw ticks "with no given time reference". The application derives frequency and offset by regression **[verified]**. `rte_eth_timesync_read_tx_timestamp(port, &ts)` is port-wide and IEEE1588-oriented, with `-EINVAL` "No timestamp is available" **[verified]**. That it is a single latched value unsuitable for per-media-packet timestamps is **[inferred]**.

---

## 8. Other streaming APIs

### 8.1 NDI **[verified: docs.ndi.video NDI-SEND]**

- `NDIlib_send_send_video_v2_async`: frame memory "will continue to be used until a synchronizing API call is made". Synchronizing calls are the next async send, a NULL frame (which flushes), a sync send, or destroy. The docs warn: free it early and the app "will most likely crash in an NDI thread",
  reuse it early and you get tearing. The recommended fix is ping-pong between two buffers. The lifetime is **implicit and positional**, with a pipeline depth of one.
- `clock_video`/`clock_audio`: submission blocks to rate-limit to the frame rate. "If you are submitting video and audio of a single thread, you should only clock one of them".
- Timecode sentinel: `NDIlib_send_timecode_synthesize` (`INT64_MAX`) means "generate for me" (UTC in 100 ns units).

### 8.2 SRT **[verified: Haivision srt docs/API/statistics.md]**

- Three stat classes. *Total* counts from socket creation. *Interval* is reset by `srt_bstats(..., clear=1)`. *Instantaneous* comes from `srt_bistats(..., instantaneous=1)`; without it, buffer stats are a **moving average**.
- Lateness vocabulary:
  - `pktSndDrop` is packets the sender dropped that "have no chance to be delivered in time".
  - `pktRcvDrop` covers never-arrived, too-late and undecryptable packets.
  - `pktRcvBelated` counts packets "received but IGNORED due to having arrived too late".
  - The TLPKTDROP threshold is defined by a formula over latency settings.
- The buffer level is expressed **in time** (`msSndBuf`, the "timespan (msec) of packets in the sender's buffer") and in packets and bytes (`pktSndBuf`, `byteAvailSndBuf`).

### 8.3 WebRTC `getStats()` **[verified: W3C webrtc-stats]**

- `framesDropped` (inbound video) counts "frames dropped prior to decode or dropped because the frame missed its display deadline". Deadline misses are a drop class.
- Jitter-buffer accounting uses **sums plus a count**: `jitterBufferDelay`, `jitterBufferTargetDelay` and `jitterBufferMinimumDelay` accumulate per emitted item. `jitterBufferEmittedCount` is the divisor.
- `freezeCount` and `totalFreezesDuration` have a precise freeze definition. `totalInterFrameDelay` has a squared variant for variance.
- The model: "A stats object, once returned, never changes". Counters "must always increase". Implementations should not compute averages; apps diff two snapshots. A counter that has not incremented yet should "report a count of zero".
- The outbound `totalPacketSendDelay` and `framesSent` definitions were truncated in my fetch **[unknown text]**. By name, the former is a sum of per-packet time from packetization to send **[inferred]**.

### 8.4 OpenTelemetry metrics **[verified: opentelemetry.io data model]**

- Sum (with a `monotonic` flag), Gauge ("last-sampled event"), Histogram (explicit buckets, `(lower, upper]`) and ExponentialHistogram.
- *Cumulative* temporality repeats the start timestamp. *Delta* advances it. The start timestamp marks resets. This is a ready-made vocabulary for MTL stats export.

---

## 9. Cross-cutting comparison

### 9.1 Output terminal results

| API | Granularity | On time | Late but sent | Not sent (lateness) | Not sent (user) | Error | Identity |
|---|---|---|---|---|---|---|---|
| DeckLink | frame | Completed | DisplayedLate | Dropped (superseded) | Flushed | — | frame object |
| AJA | aggregate | framesProcessed++ | — | framesDropped++ | Flush (no per-frame) | transfer returns false | cookie polled |
| GStreamer basesink | buffer | rendered | rendered if lateness ≤ max-lateness | dropped + QoS msg | flush events | flow error | buffer, running-time |
| Vulkan present timing | present | actual ≈ target | actual > target (margin) | — (not modelled) | — | VkResult | presentId |
| io_uring | op | res ≥ 0 | — | -ETIME (link timeout) | -ECANCELED | -errno | user_data |
| CoreAudio | IO cycle | — | overload notification | — | — | — | none |
| JACK | cycle | — | xrun callback | — | — | — | none |
| NDI async | frame | implicit | — | — | — | — | none |
| MTL today | frame | notify_frame_done(COMPLETE) | — | notify_frame_done(DROPPED); notify_frame_late(epoch_skipped) | — | — | frame_idx + meta |

The MTL row was checked in `include/st20_api.h` (`notify_frame_done`, `notify_frame_late(void* priv, uint64_t epoch_skipped)`) and `include/st_api.h` (`st_frame_status`, `stat_frames_dropped`) **[verified: repo]**.

### 9.2 Timing fields in results

| API | Requested | Resolved/scheduled | Observed | Margin | Clock identity | Validity |
|---|---|---|---|---|---|---|
| DeckLink | displayTime (stream) | — | completion ref timestamp (system clock, callback-only) | — | implicit | method return code |
| AJA | — | — | acCurrentFrameTime and cookie | acStartSample drift | host plus 48 kHz device clock pair | 0xFFFFFFFF "not available" |
| Vulkan GOOGLE | desiredPresentTime | earliestPresentTime | actualPresentTime | presentMargin | implicit (monotonic) | — |
| Vulkan EXT | targetTime | — | per-stage times | derivable | timeDomain + timeDomainId | time 0, reportComplete |
| ALSA | — | — | tstamp and audio_tstamp | delay/avail | clock mode + actual_type | valid, accuracy_report bits |
| CoreAudio | inOutputTime (given to app) | — | — | — | multiple reps | mFlags bits |
| GStreamer | PTS→running-time | — | clock time at render | jitter (signed) | caps in reference meta | GST_CLOCK_TIME_NONE |

---

## 10. Patterns MTL should adopt

Each item has a one-line rationale and the review guarantee (§14) it supports, where one applies.

1. **A per-submission terminal result enum that separates late-but-sent from not-sent, plus a reason code.** Proposed values: `ON_TIME`, `LATE` (sent, but first packet after deadline + tolerance), `DROPPED` (never sent; reason `TOO_LATE`, `SUPERSEDED`, `NO_LINK`, ...), `FLUSHED` (user stop, abort or
   drain) and `FAILED` (errno). *Rationale:* DeckLink proves these four outcomes are distinct and actionable, and MTL's current `DROPPED` plus an out-of-band `notify_frame_late(epoch)` loses the late-but-sent case and the identity (§14.1, §14.25).
2. **Requested vs resolved vs observed times plus margin in every TX result.** Fields: requested media time and epoch; resolved deadline (TPR0/TAI); scheduled first-packet time; observed first and last packet times; `margin_ns = deadline − time_accepted_by_tasklet` (signed). *Rationale:* Vulkan's
   `presentMargin` and `earliestPresentTime` answer "how close was I" and let apps tune their lead time, which a boolean cannot (§14.17).
3. **A clock-domain tag plus validity bits on every timestamp, captured as a coherent snapshot.** Shape: `{ value_ns; clock (TAI_PTP, TAI_SW_ESTIMATE, NIC_PHC_RAW, TSC, MONOTONIC); valid_mask; accuracy_ns (optional, with a valid bit) }`. *Rationale:* CoreAudio flags, ALSA
   `actual_type`/`accuracy_report`, Vulkan `timeDomain` and GStreamer reference caps all avoid "0 means unknown" and "which clock?" ambiguity (§14.15, §14.16).
4. **A completion queue that cannot overflow, by construction.** Reserve the result slot at submit time: capacity is at least the maximum number of accepted in-flight submissions, and submit fails (`-EAGAIN`/`-ENOBUFS`) when the reservation fails. *Rationale:* this is Vulkan's `QUEUE_FULL` and
   DeckLink's `E_OUTOFMEMORY`. io_uring's overflow list needs allocation, which is illegal in the data plane (§14.4).
5. **Separate "transport outcome" from "storage reusable" only when they genuinely differ, and say which one is terminal.** Either one terminal result posted after both (review §9 v1), or io_uring-style `RESULT` plus `NOTIF` with a flag saying "more coming for this submission". *Rationale:* io_uring
   `SEND_ZC` shows the two-event model is workable and explicit, and DPDK's lazy mbuf free means the two moments really do differ (§14.6).
6. **An opaque 64-bit user cookie round-trip on every submission, result and RX delivery.** *Rationale:* io_uring `user_data`, AJA `acInUserCookie`, Vulkan `presentId` and PipeWire `user_data` all use it, and slot indices are not identity (§14.8).
7. **Buffer-level gauges in both count and time, with low and high watermarks.** Report queued submissions and ns of media queued ahead of the wire, with low/high watermarks per stats epoch. *Rationale:* live latency is a time quantity. DeckLink and AJA give counts, SRT gives time span, ALSA's
   `avail_max` and PipeWire's `queued_buffers`/`avail_buffers` show watermarks. An underrun is foreseeable when the level trends to 0.
8. **Pull pacing that tells the producer which slot it is filling.** Acquire (or a `wait_slot` call) returns the target epoch/TAI and deadline of the next free slot. *Rationale:* OpenXR `predictedDisplayTime`, CoreAudio `inOutputTime` and JACK `next_usecs` let the producer render for the right time rather than guess.
9. **Preroll and start-at-time as explicit lifecycle steps.** Submits are accepted in a stopped or armed state. `start(at_tai)` releases them. `stop(at_media_time)` returns the actual stop time and FLUSHES later submissions. A group start covers sessions that must begin together. *Rationale:* DeckLink scheduled start/stop and playback groups, and AJA `STARTING_AT_TIME` (§14.20, §14.24).
10. **Advertise latency as a range and reject impossible configurations at setup.** Each session reports `{min_ns, max_ns}` added latency (pacing lead, pipeline conversion, RX reassembly or jitter window). A deadline configuration that cannot be met is an error at create or start. *Rationale:* the
    GStreamer latency query and JACK latency ranges, and the GStreamer plugin (`ecosystem/gstreamer_plugin`) could answer LATENCY queries from this directly.
11. **Flag RX anomalies per frame instead of breaking cadence.** RX delivers or signals every expected slot with flags such as `NO_SOURCE`, `INCOMPLETE`, `RECONSTRUCTED` or `LATE`. Format or SDP changes become an event carrying a bitmask of which properties changed plus the new format. *Rationale:*
    DeckLink `bmdFrameHasNoInputSource` and `VideoInputFormatChanged(events bitmask, newMode, flags)` let apps keep A/V timing while knowing a frame is invalid.
12. **Stats as immutable snapshots of monotonic cumulative counters starting at 0, plus separate gauges and histograms.** No moving averages: expose sums and counts (for example `sum_margin_ns`, `count`). Include a snapshot timestamp and a stats-epoch start time instead of reset-on-read.
    *Rationale:* WebRTC's explicit guidance and OTel's cumulative temporality, and multiple readers (app, GStreamer element, exporter) can coexist.
13. **Key-addressed extension stats for rare or vendor items.** Use `mtl_stat_get_int(handle, stat_id, param, &value)` alongside the fixed struct. *Rationale:* DeckLink `IDeckLinkStatistics::GetInt[WithParam]` grows without breaking ABI (PTP loss-of-lock, per-port drops).
14. **Coalesced informational events that carry counts since the last event and the last identity.** For example `{type = RX_PKT_LOSS, count_since_last, last_seq, last_ts}`, rate-limited. Ownership results are never coalesced. *Rationale:* the GStreamer jitterbuffer "drop-msg" with `num-too-late`
    "since last drop message" plus `drop-messages-interval`. It keeps event volume bounded without losing information (review §9 rule 10).
15. **ABI evolution: `struct_size` first, a version field in callback tables, and a size-parameterized getter.** Reserve an sType/pNext-style chain only for optional extension blocks on create-time config. *Rationale:* PipeWire `get_time_n(size)` and `events.version`, AJA `fSizeInBytes`+`fVersion`, and Vulkan's "skip unknown sType" rule for composability (§14.27).
16. **Report the actual mode chosen when it differs from the request, and never degrade exact requests silently.** Examples: timestamp source actually used, pacing mode, data path. *Rationale:* ALSA `actual_type` and Vulkan's substitute `timeDomainId`. The review adds "exact requests fail, never fall back" (review §11).
17. **An explicit poller-sleep flag and wake protocol if tasklets can sleep.** *Rationale:* io_uring `IORING_SQ_NEED_WAKEUP` / `IORING_ENTER_SQ_WAKEUP` makes the "submit while poller idles" latency visible and cheap to fix.
18. **Deferred release notification for memory unregister, as an option alongside `-EBUSY`.** *Rationale:* io_uring resource tags post a CQE "after the resource had been unregistered and it's not used anymore", which avoids polling loops at teardown (§14.10).
19. **Keep transport (flow and SDP) configuration separable from media I/O.** *Rationale:* DeckLink IP reuses the unchanged SDI media API and puts 2110 flows, SDP and NMOS in a separate object family. This matches MTL's "media-polymorphic verbs" goal (review §13).

---

## 11. Patterns to avoid

1. **Results or timestamps valid only inside a callback.** *Rationale:* DeckLink `GetFrameCompletionReferenceTimestamp` is valid only in `ScheduledFrameCompleted`, and only before reschedule. Put everything in a retained, copyable result record instead.
2. **Implicit, positional buffer lifetime ("valid until the next call").** *Rationale:* NDI async send caps pipeline depth at 1, forces ping-pong, and crashes on misuse. Lifetime must end at an explicit terminal result.
3. **Information-free exception callbacks.** *Rationale:* JACK's xrun callback has no arguments, and MTL's `notify_frame_late(priv, epoch_skipped)` names no submission, deadline or observed time. Apps cannot correlate or react precisely.
4. **Aggregate-only drop accounting.** *Rationale:* AJA `acFramesDropped` tells you *that* something dropped, not *which* frame or *why*. Counters must complement per-submission results, not replace them.
5. **Trusting the NIC or PMD to report lateness.** *Rationale:* DPDK says past launch timestamps "should be ignored", so late packets leave immediately and silently. MTL must compare against the deadline itself, before enqueue.
6. **Using mbuf or extbuf free as "transmitted at" time.** *Rationale:* DPDK frees lazily per `tx_free_thresh`. Release time is reuse time, not wire time, so report observed wire time separately and label its source.
7. **Overflow storage that allocates, or "no drop unless OOM".** *Rationale:* io_uring NODROP still has `-EBADR`. MTL forbids allocation in the data plane, so bound in-flight work instead.
8. **Implementation-computed moving averages and implicit windows.** *Rationale:* SRT buffer stats default to a moving average, and GStreamer basesink `average-rate` has an unspecified window. Readers cannot combine or diff them.
9. **Reset-on-read or global-reset counters as the primary model.** *Rationale:* ALSA `avail_max`, SRT `clear` and AJA `inClearDropCount` break as soon as two consumers read, and MTL already has `*_reset_session_stats` with the same problem. Prefer cumulative counters plus per-reader deltas, and keep reset as an explicit "new stats epoch" with a start timestamp.
10. **Heap allocation or other hidden work in the per-frame path.** *Rationale:* AJA's own `@bug` note on S2110 anc handling. It is also the review's two-world rule.
11. **Coupling correctness to callback execution time.** *Rationale:* DeckLink warns that callback work over a frame time causes drops. In MTL, callbacks run in tasklet context with µs budgets, so queue-based completion delivery should be primary.
12. **Only stream-relative or "absolute values are meaningless" clocks for scheduling.** *Rationale:* DeckLink's stream time and hardware clock suit genlocked SDI. ST 2110 needs absolute TAI epochs on every submission and result.
13. **Magic sentinel values inside data fields.** *Rationale:* NDI `INT64_MAX` "synthesize", Vulkan "time 0 = unavailable" and AJA `0xFFFFFFFF` "not available" all collide with legitimate values. Use validity bits (§14.16).
14. **Blocking transfer calls that combine admission, copy and result.** *Rationale:* AJA `AutoCirculateTransfer` blocks the caller thread and returns status in place. Fine for SDI DMA, but it prevents deep async pipelines and a multi-session event loop.

---

## 12. Mapping to the review's object model

| Review concept (§7–§9) | Closest prior art |
|---|---|
| memory region, registration | AJA `DMABufferLock`; io_uring `REGISTER_BUFFERS` (+ tags); DeckLink allocator provider |
| buffer (immutable layout) | DeckLink `IDeckLinkVideoBuffer` + `CreateVideoFrameWithBuffer`; GStreamer `GstBuffer`/`GstMemory` |
| access lease | DeckLink `StartAccess/EndAccess(flags)`; Vulkan release/acquire barriers **[inferred]** |
| submission (per-use timing) | DeckLink `ScheduleVideoFrame(displayTime)`; Vulkan `VkPresentTimingInfoEXT`; AJA `AUTOCIRCULATE_TRANSFER` |
| terminal result, exactly once | DeckLink `ScheduledFrameCompleted`; io_uring CQE; Vulkan past-presentation record |
| no-drop completion queue | Vulkan reserved result slot + `QUEUE_FULL`; io_uring NODROP (with caveats) |
| flush/drain/abort | DeckLink `Flushed` + `StopScheduledPlayback(at, &actual)`; AJA `Flush` / `Stop(abort)`; io_uring cancel + `-ECANCELED` |
| RX dequeue/release | GStreamer pool return-on-unref; io_uring provided-buffer ring; DeckLink `AddRef/Release` on input frame |

---

## Open questions for the maintainer

1. **What exactly is "late" on TX, and against which reference point?** *Why it matters:* a `LATE` result is only useful if everyone agrees on the threshold, and ST 2110-21 conformance is measured at the wire. *Options:* (a) the first packet leaves after TPR0 + TR_offset (the ST 2110-21 notion); (b)
   the submission reached the TX tasklet after the latest time it could still meet (a) (this is the admission deadline, and gives a pre-send verdict); (c) both, reporting (b) as `margin_ns` and (a) as the observed verdict. Each option also needs a per-session tolerance.
2. **When a frame misses its slot, is it sent late, sent in the next slot, or dropped?** *Why it matters:* DeckLink sends the "least late" frame and drops superseded ones. MTL today skips epochs. Live broadcast users often prefer a repeat to a shift. *Options:* a per-session policy enum
   `DROP_IF_LATE`, `NEXT_EPOCH` (shifts later media, which review §14.21 forbids for synced streams) or `REPEAT_PREVIOUS` (needs retained previous buffer). Decide which is default and which are capabilities.
3. **One terminal result, or result plus a buffer-reusable notification (io_uring `SEND_ZC`)?** *Why it matters:* on the direct path, NIC descriptors can hold references well after the last packet's launch time, which adds latency to buffer reuse and inflates pool size. *Options:* (a) a single
   terminal result after both (simplest, review §9 v1); (b) two events with a "more coming" flag; (c) a single result, plus a separate `observed_wire_time` so timing is not delayed by reclaim.
4. **How is completion-queue capacity chosen, and what happens when it is full?** *Why it matters:* a no-drop guarantee requires bounded in-flight work. *Options:* (a) capacity equals pool size, so full is impossible by construction; (b) app-sized like Vulkan, with submit returning `-EAGAIN` when no
   result slot can be reserved; (c) a separate bounded informational-event queue with overflow counters and coalescing. (a)+(c) looks like the minimal combination.
5. **How are completions delivered: dequeue only, callbacks, or both, and in which thread context?** *Why it matters:* DeckLink and CoreAudio callbacks impose deadlines on user code, which in MTL means inside a tasklet. *Options:* (a) dequeue/poll/wait only, plus an eventfd for epoll integration;
   (b) a tasklet-context callback restricted to non-blocking work; (c) a library-owned callback thread. Also decide whether results for one session are FIFO in media-time order or in any order (io_uring-style), with the cookie as identity.
6. **Which "observed" timestamps can MTL actually provide on E810/E830, with what accuracy, and how are they labelled?** *Why it matters:* adopting validity-flagged, clock-tagged times (pattern 3) is only honest if sources are known. *Options:* per-packet HW TX timestamps (probably unavailable for
   media packets **[inferred]**); rate-limiter or launch-time-derived estimates; TSC at the `tx_burst` return. Each needs a `clock` tag and an `accuracy_ns`, or "unknown".
7. **What units do buffer-level gauges use, and what are the watermark semantics?** *Why it matters:* live users tune latency in ms, not frame counts, and a pure count hides the difference between 1080p59.94 and 2160p25. *Options:* count only; count plus ns-ahead-of-wire; plus low/high watermarks
   per stats epoch. Also decide whether RX exposes the same (frames ready but not dequeued, and the age of the oldest).
8. **Should MTL move from reset-able stats to cumulative-only snapshots with stats epochs?** *Why it matters:* the existing `*_reset_session_stats()` is reset-on-demand, which breaks multiple concurrent readers (app plus GStreamer element plus exporter). *Options:* (a) cumulative only, with apps
   diffing; (b) cumulative plus an explicit "new epoch" with a start timestamp (the OTel model); (c) keep reset for compatibility but deprecate it in the unified API.
9. **Is `struct_size` alone enough, or is an sType/pNext chain needed for extension blocks?** *Why it matters:* media types will keep adding optional features (HDR metadata, progressive publish, ST 2022-7 details). Growing one struct by `struct_size` couples unrelated extensions. *Options:* (a)
   `struct_size` everywhere (the review's current choice); (b) `struct_size` plus an optional typed `next` chain on config and result structs only; (c) AJA-style header with tag, type, size and version. The choice also determines how results report fields the caller did not know about.
10. **Should sessions advertise a latency range and a minimum lead time, and validate deadline configurations at start?** *Why it matters:* GStreamer (the in-tree plugin) needs min/max latency for live pipelines, and apps need to know how early to submit. *Options:* (a) static per-session
    `{min_lead_ns, max_queue_ns}` from config; (b) a dynamic range updated with a "latency changed" event; (c) none, leaving apps to infer from `margin_ns` histograms.
11. **What does RX deliver when the source is absent or late: nothing, a flagged placeholder, or a timeout result?** *Why it matters:* A/V sync and downstream cadence are easier with a result per expected slot (DeckLink `bmdFrameHasNoInputSource`), but that costs buffers. *Options:* (a) deliver only
    real frames plus a coalesced `NO_SOURCE` event; (b) deliver a flagged empty result per expected slot, without a buffer; (c) make it configurable.
12. **Is preroll/armed start plus a multi-session group start in scope for v1?** *Why it matters:* frame-accurate start of video, audio and anc together is a common broadcast requirement (DeckLink playback groups, AJA `STARTING_AT_TIME`). It also fixes the first-frame lateness that live pipelines
    otherwise see. *Options:* (a) `start(at_tai)` per session only; (b) a group object with an atomic arm and start; (c) defer to v2 but reserve the API shape now.

---

## Sources

All accessed 2026-09-29.

- **DeckLink** — Blackmagic Design, *DeckLink SDK Manual*, March 2026: <https://documents.blackmagicdesign.com/UserManuals/DeckLinkSDKManual.pdf> (§2.4.2 Playback, §2.4.15 SMPTE 2110 IP Flows, §2.5.3.9–30 IDeckLinkOutput, §2.5.6 IDeckLinkVideoOutputCallback, §2.5.10–11 input, §2.5.53–55 video buffer
  and allocator, §2.5.59 IP flow, §2.5.63 statistics, §3.9 completion results). SDK 12.0 header: <https://github.com/obsproject/obs-studio/blob/master/plugins/decklink/linux/decklink-sdk/DeckLinkAPI.h>. SDK 12.2.2 header:
  <https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/main/subprojects/gst-plugins-bad/sys/decklink/linux/DeckLinkAPI.h>
- **AJA** — <https://github.com/aja-video/libajantv2/blob/main/ajantv2/includes/ntv2publicinterface.h> (`NTV2AutoCirculateState`, `NTV2_HEADER`, `NTV2_TRAILER`, `AUTOCIRCULATE_STATUS`, `FRAME_STAMP`, `AUTOCIRCULATE_TRANSFER_STATUS`, `AUTOCIRCULATE_TRANSFER`) and <https://github.com/aja-video/libajantv2/blob/main/ajantv2/includes/ntv2card.h> (`DMABufferLock`, `AutoCirculate*`)
- **GStreamer** — design docs: <https://gstreamer.freedesktop.org/documentation/additional/design/qos.html>, <https://gstreamer.freedesktop.org/documentation/additional/design/latency.html>, <https://gstreamer.freedesktop.org/documentation/additional/design/synchronisation.html>,
  <https://gstreamer.freedesktop.org/documentation/additional/design/bufferpool.html>. API reference: <https://gstreamer.freedesktop.org/documentation/base/gstbasesink.html>. Sources: <https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/main/subprojects/gstreamer/gst/gstbuffer.h>,
  <https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/main/subprojects/gst-plugins-good/gst/rtpmanager/gstrtpjitterbuffer.c>
- **io_uring** — <https://github.com/torvalds/linux/blob/master/include/uapi/linux/io_uring.h>, <https://man7.org/linux/man-pages/man7/io_uring.7.html>, <https://man7.org/linux/man-pages/man2/io_uring_setup.2.html>, <https://man7.org/linux/man-pages/man2/io_uring_enter.2.html>, <https://man7.org/linux/man-pages/man2/io_uring_register.2.html>
- **Vulkan** — <https://github.com/KhronosGroup/Vulkan-Headers/blob/main/include/vulkan/vulkan_core.h>, <https://github.com/KhronosGroup/Vulkan-Docs/blob/main/chapters/fundamentals.adoc> (pNext skip rule). Refpages:
  <https://docs.vulkan.org/refpages/latest/refpages/source/VkPastPresentationTimingGOOGLE.html>, <https://docs.vulkan.org/refpages/latest/refpages/source/VkPastPresentationTimingEXT.html>, <https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentTimingInfoEXT.html>,
  <https://docs.vulkan.org/refpages/latest/refpages/source/vkSetSwapchainPresentTimingQueueSizeEXT.html>, <https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html>
- **OpenXR** — <https://registry.khronos.org/OpenXR/specs/1.1/man/HTML/xrWaitFrame.html>, <https://registry.khronos.org/OpenXR/specs/1.1/man/HTML/XrFrameState.html>, <https://registry.khronos.org/OpenXR/specs/1.1/man/HTML/xrBeginFrame.html>
- **ALSA** — <https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html>, <https://github.com/torvalds/linux/blob/master/Documentation/sound/designs/timestamping.rst>, <https://github.com/torvalds/linux/blob/master/include/uapi/sound/asound.h>, <https://github.com/alsa-project/alsa-lib/blob/master/include/pcm.h>
- **JACK** — <https://github.com/jackaudio/headers> (`jack.h`, `types.h`, `statistics.h`)
- **PipeWire** — <https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/master/src/pipewire/stream.h>
- **CoreAudio** — MacOSX 11.3 SDK headers, via mirror <https://github.com/phracker/MacOSX-SDKs> (`CoreAudioTypes.framework/Headers/CoreAudioBaseTypes.h`, `CoreAudio.framework/Headers/AudioHardware.h`, `AudioHardwareBase.h`)
- **DPDK** — <https://github.com/DPDK/dpdk/blob/main/lib/mbuf/rte_mbuf_dyn.h>, <https://github.com/DPDK/dpdk/blob/main/lib/ethdev/rte_ethdev.h>, <https://github.com/DPDK/dpdk/blob/main/lib/mbuf/rte_mbuf_core.h>
- **NDI** — <https://docs.ndi.video/all/developing-with-ndi/sdk/ndi-send>
- **SRT** — <https://github.com/Haivision/srt/blob/master/docs/API/statistics.md>
- **WebRTC** — <https://www.w3.org/TR/webrtc-stats/>
- **OpenTelemetry** — <https://opentelemetry.io/docs/specs/otel/metrics/data-model/>
- **MTL (current API, for comparison)** — `include/st_api.h` (`st_frame_status`, `st_tx_user_stats`), `include/st20_api.h` (`notify_frame_done`, `notify_frame_late`), `include/st_pipeline_api.h` (`*_reset_session_stats`)
