# 09 — libfabric (OFI) API design, and how it maps to MTL

| | |
|---|---|
| Topic | libfabric object model, discovery, memory registration, data ops, completion, events, progress/threading, cancel/close, ABI; mapping to a redesigned MTL public API |
| Date | 2026-09-29 |
| libfabric version examined | `main` branch, `FI_MAJOR_VERSION 2`, `FI_MINOR_VERSION 7` (`include/rdma/fabric.h`); `NEWS.md` lists v2.7.0 as "Fri Sept 18, 2026" |
| Primary sources | Man pages at `https://ofiwg.github.io/libfabric/main/man/`: `fabric.7`, `fi_arch.7`, `fi_intro.7`, `fi_setup.7`, `fi_provider.7`, `fi_getinfo.3`, `fi_domain.3`, `fi_endpoint.3`, `fi_mr.3`, `fi_cq.3`, `fi_eq.3`, `fi_cntr.3`, `fi_poll.3`, `fi_msg.3`, `fi_cm.3`, `fi_av.3`, `fi_trigger.3`, `fi_peer.3`, `fi_version.3` |
| Headers | `https://raw.githubusercontent.com/ofiwg/libfabric/main/include/rdma/{fabric.h,fi_domain.h,fi_endpoint.h,fi_errno.h}`, `src/abi_1_0.c`, `NEWS.md` |
| Wiki | `github.com/ofiwg/libfabric/wiki`: "ABI Versions", "Common OFI Mistakes to Avoid", "Provider Feature Matrix v2.7.x" |
| MTL files consulted | `include/mtl_api.h`, `include/st_api.h`, `include/st20_api.h`, `include/st_pipeline_api.h`, `include/mtl_sch_api.h` |
| Access notes | All man pages and headers above were fetched successfully. `libfabric.map.in` and `fi_errno.3` did not render; `fi_errno.h` was used instead. No `fi_hmem(3)` page was used — HMEM is covered through `fi_mr(3)` and `fi_getinfo(3)`. |

Labels: **[verified]** = stated in the cited primary source; **[inferred]** = my reading or background knowledge, not checked against a source in this session; **[unknown]** = could not establish.

---

## 0. TL;DR for the synthesizer

- libfabric's durable ideas are small and cheap:
  - **opaque handles plus an ops table with a `size` field**;
  - a **user `context` pointer returned verbatim in every completion**;
  - **polled completion queues separate from a control-plane event queue**;
  - **explicit completion levels that say exactly when a buffer may be reused**;
  - **`-EAGAIN` as the only backpressure signal**;
  - **memory registration returning a descriptor that is passed with each op**;
  - an **explicit version argument at the entry point**.
- libfabric's expensive ideas come from serving about 14 heterogeneous providers: mode bits (provider asks the app for help), `mr_mode` combinatorics, five threading models (two now deprecated), wait sets and poll sets (deprecated in 2.0), per-provider feature variance, fatal CQ overrun, and silent discard of in-flight ops on close.
  MTL has one implementation with a few backends, so it should not copy these.
- The analogy breaks where time matters. libfabric has **no notion of time**: no scheduled transmit, no deadline, no "late" completion, and no timestamp in completions. Its triggers are counter thresholds only. MTL's core semantics (epoch-paced TX, PTP timestamps, late or dropped frames, ST 2022-7 redundancy) must be designed from scratch; libfabric only shapes the plumbing.

---

## 1. Object hierarchy and the ops-table pattern

### 1.1 Objects

| Object | Role | Source |
|---|---|---|
| `fid_fabric` | "hardware and software resources that access a single physical or virtual network"; top-level object | [verified] `fabric.7`, `fi_arch.7` |
| `fid_domain` | "a single logical connection into a fabric", such as a NIC or port; owns MR, AV, CQ, counters, endpoints | [verified] `fabric.7` |
| `fid_ep` (active), `fid_pep` (passive) | "a communication portal"; passive endpoints listen, active ones transfer data. Types: `FI_EP_MSG` (reliable, connected), `FI_EP_DGRAM` (unreliable, connectionless), `FI_EP_RDM` (reliable, connectionless) | [verified] `fabric.7`, `fi_endpoint.3` |
| `fid_cq` | "high-performance event queues" for data-transfer completions | [verified] `fabric.7` |
| `fid_eq` | asynchronous events not tied to data transfers (CM, MR, AV, async errors) | [verified] `fabric.7`, `fi_eq.3` |
| `fid_cntr` | lightweight completion: a completion "simply increments a counter" | [verified] `fabric.7` |
| `fid_mr` | grants the provider access to application buffers | [verified] `fabric.7` |
| `fid_av` | maps addresses (e.g. IP) to compact `fi_addr_t` values used on the fast path | [verified] `fabric.7`, `fi_av.3` |
| `fid_mc` | multicast group membership (`fi_join`) | [verified] `fi_cm.3` |
| wait set, poll set | aggregate waiting and progress across CQs and counters; **deprecated in 2.0** | [verified] `fi_poll.3`, `NEWS.md` v2.0.0 alpha "Deprecate wait set and poll set" |
| scalable EP, shared TX/RX contexts | one address with many HW queues; one queue serving many endpoints | [verified] `fi_endpoint.3` |

Lifecycle is **open, then bind, then enable**. "CQs, counters, EQs and AVs must be bound before enabling", and "An endpoint must be enabled before it may be used to perform data transfers." `fi_setopt` "must be called before `fi_enable`" [verified] `fi_endpoint.3`. Unconnected endpoints must be bound to an AV; connected endpoints need an EQ [verified] `fi_setup.7`.

### 1.2 `fid` and ops tables

Every object begins with the same header:

```c
/* All fabric interface descriptors must start with this structure */
struct fid { size_t fclass; void *context; struct fi_ops *ops; };

struct fi_ops {
  size_t size;
  int (*close)(struct fid *fid);
  int (*bind)(struct fid *fid, struct fid *bfid, uint64_t flags);
  int (*control)(struct fid *fid, int command, void *arg);
  int (*ops_open)(struct fid *fid, const char *name, uint64_t flags, void **ops, void *context);
  int (*tostr)(const struct fid *fid, char *buf, size_t len);
  int (*ops_set)(struct fid *fid, const char *name, uint64_t flags, void *ops, void *context);
};
```

[verified] `fabric.h`. Objects carry per-class tables, e.g. `struct fid_ep { struct fid fid; struct fi_ops_ep *ops; struct fi_ops_cm *cm; struct fi_ops_msg *msg; struct fi_ops_rma *rma; struct fi_ops_tagged *tagged; struct fi_ops_atomic *atomic; struct fi_ops_collective *collective; }` [verified] `fi_endpoint.h`.

- **Dispatch is static inline through function pointers.** `static inline ssize_t fi_send(...) { return ep->msg->send(ep, buf, len, desc, dest_addr, context); }` [verified] `fi_endpoint.h`. So libfabric "only exports a handful of functions directly", and ABI concerns are "most notably the fi_getinfo call and its returned attribute structures" [verified] `fabric.7`.
- **Tables grow at the tail; new ops are guarded by `size`.**
  - Guard: `#define FI_CHECK_OP(ops, opstype, op) (ops && (ops->size > offsetof(opstype, op)) && ops->op)` [verified] `fabric.h`.
  - Example: `fi_domain2()` falls back to `fi_domain()` when `flags == 0`; otherwise it calls `domain2` only if `FI_CHECK_OP` passes, else it returns `-FI_ENOSYS` [verified] `fi_domain.h`.
  - Tail growth in `fi_ops_domain`: `query_atomic`, then `query_collective`, then `endpoint2`, then `xpu_ctx` [verified] `fi_domain.h`.
  - "the size field allows the fi_ops structure to grow in a backward compatible manner as new operations are added" [verified] `fi_setup.7`.
- **Extensions:** `fi_open_ops(fid, name, flags, &ops, ctx)` "is used to open provider specific interfaces", which can expose low-level, domain-specific resources [verified] `fi_domain.3`, `fabric.h`. `fi_set_ops` lets the caller *install* ops (used for overrides) [verified] `fabric.h`. `fi_control(fid, cmd, arg)` is the generic ioctl, e.g. `FI_GETWAIT` [verified] `fi_cq.3`, `fi_eq.3`.
- **Fast path, not fast-path-checked.** Data wrappers such as `fi_send` do not check for NULL ops; only newer optional ops go through `FI_CHECK_OP` [verified] `fi_endpoint.h`.
- `FABRIC_DIRECT` is a compile-time mode that links an app directly against one provider and removes the indirection. It appears as a row in the provider feature matrix [verified] wiki matrix; mechanism details [inferred].

**MTL relevance.** MTL is a single implementation, so a vtable per object buys ABI evolution and backend polymorphism (DPDK PMD, AF_XDP, kernel socket, and possibly media types), not provider plug-ins. The part to copy is the size-guarded tail growth plus exported non-inline entry points.
Inline vtable dispatch would freeze the struct layout into applications, which is the opposite of what MTL needs [inferred].

---

## 2. Discovery and capabilities: `fi_getinfo`

```c
int fi_getinfo(int version, const char *node, const char *service,
               uint64_t flags, const struct fi_info *hints, struct fi_info **info);
```

`fi_info` fields are `next, caps, mode, addr_format, src_addrlen, dest_addrlen, src_addr, dest_addr, handle, tx_attr, rx_attr, ep_attr, domain_attr, fabric_attr, nic` [verified] `fi_getinfo.3`, `fabric.h`.

### 2.1 Negotiation rules

- **Non-zero hint means required.** "specifying a non-zero value for input hints indicates that a provider must support the requested value or fail the operation" [verified] `fi_getinfo.3`.
- **Zero means wildcard,** except for mode bits: "hints that are set to zero are treated as a wildcard", and zeroed values yield "a default value or a value that works best for their implementation" [verified] `fi_getinfo.3`.
- **Caps are a floor.** "Providers may indicate support for additional capabilities beyond those requested", but only if extra caps do not hurt performance or widen communication. Apps may "request a minimal set of requirements, then check the returned capabilities" [verified] `fi_getinfo.3`.
- **Attributes are floors too.** For tx/rx/ep/domain attrs, "Output values will be greater than or equal to requested input values" [verified] `fi_getinfo.3`.
- **Caps come in three tiers.**
  - Primary (`FI_MSG, FI_RMA, FI_TAGGED, FI_ATOMIC, FI_MULTICAST, FI_HMEM, FI_COLLECTIVE, ...`) "must explicitly be requested".
  - Modifiers (`FI_SEND/RECV/READ/WRITE/REMOTE_*`): if none is specified, all are assumed.
  - Secondary (`FI_SOURCE, FI_RMA_EVENT, FI_SHARED_AV, FI_TRIGGER, FI_FENCE, FI_LOCAL_COMM, ...`): if requested, "must support ... or fail (FI_ENODATA)".
  - [verified] `fi_getinfo.3`.
- **Mode bits are the reverse channel.** "capability bits are top down requests, whereas mode bits are bottom up restrictions" [verified] `fi_setup.7`.
  - The app sets the modes it can tolerate, and "providers will clear mode bits that are not necessary" [verified] `fi_getinfo.3`.
  - "If a provider requires a mode bit that isn't set, that provider will be skipped" [verified] `fi_setup.7`.
- **Mode examples** [verified] `fi_getinfo.3`:
  - `FI_CONTEXT` / `FI_CONTEXT2`: the app must pass a provider-scratch `struct fi_context` ("should NOT be allocated on the stack") as the per-op context.
  - `FI_MSG_PREFIX`: the app reserves header space ahead of buffers.
  - `FI_ASYNC_IOV`: the iovec must stay untouched until completion.
  - `FI_RX_CQ_DATA`.
  - `FI_LOCAL_MR`: deprecated, replaced by `mr_mode`.
- **Result order.** A linked list is returned, best performing first: "should return the endpoints that are highest performing first" [verified] `fi_getinfo.3`.
- **Housekeeping.**
  - `FI_PROV_ATTR_ONLY` lists providers without probing hardware.
  - Hints "must come from `fi_allocinfo()` or `fi_dupinfo()`" (2.0 "Require using libfabric APIs to allocate fi_info structures").
  - [verified] `fi_getinfo.3`, `NEWS.md`.

### 2.2 Attribute groups (selected)

| Group | Notable fields | Source |
|---|---|---|
| `domain_attr` | `threading`, `control_progress`, `progress`/`data_progress` (union alias), `resource_mgmt`, `av_type`, `mr_mode`, `mr_key_size`, `cq_cnt`, `ep_cnt`, `tx_ctx_cnt`, `rx_ctx_cnt`, `cntr_cnt`, `mr_iov_limit`, `max_err_data`, `mr_cnt`, `tclass`, `max_cntr_value`, ... | [verified] `fabric.h` |
| `tx_attr` / `rx_attr` | `size` (queue depth), `iov_limit`, `inject_size`, `op_flags` (default flags), `msg_order`, `comp_order` (deprecated) | [verified] `fi_endpoint.3` |
| `fabric_attr` | `name`, `prov_name`, `prov_version`, `api_version` | [verified] `fabric.h` |

`cq_cnt`, `tx_ctx_cnt` and similar report the *optimal* count, not a hard limit: "Allocating more means sharing underlying resources" [verified] `fi_domain.3`. Message size limits are exposed rather than segmented internally: "Providers expose their hardware or network limits to the applications, rather than segmenting large transfers internally" [verified] `fi_setup.7`.

**MTL relevance.**

- MTL currently has no "requested vs granted" channel. Several features silently degrade [inferred from MTL headers]:
  - `ST20P_RX_FLAG_DMA_OFFLOAD` "could fallback to CPU if no DMA device is available" [verified] `include/experimental/st20_combined_api.h`, same wording in `st_pipeline_api.h`.
  - Pacing falls from rate-limit to TSC [inferred from CLAUDE.md "software pacing fallback"].
- The libfabric rule is the one worth copying: **non-zero request means must-have or fail; zero means library chooses; the chosen value is reported back.**
- A granted-attributes output struct from session create, or a `*_get_attr()` query afterwards, is the minimal form.
- Mode bits are not worth copying. MTL is the only implementer, so it can just satisfy its own restrictions internally [inferred].

---

## 3. Memory registration

```c
int fi_mr_reg(struct fid_domain *d, const void *buf, size_t len, uint64_t access, uint64_t offset,
              uint64_t requested_key, uint64_t flags, struct fid_mr **mr, void *context);
int fi_mr_regv(... const struct iovec *iov, size_t count, ...);
int fi_mr_regattr(struct fid_domain *d, const struct fi_mr_attr *attr, uint64_t flags, struct fid_mr **mr);
void *fi_mr_desc(struct fid_mr *mr);   uint64_t fi_mr_key(struct fid_mr *mr);
int fi_mr_refresh(struct fid_mr *mr, const struct iovec *iov, size_t count, uint64_t flags);
```

[verified] `fi_mr.3`. `struct fid_mr { struct fid fid; void *mem_desc; uint64_t key; }` [verified] `fi_domain.h`.

- **`fi_mr_attr`** holds a union of `mr_iov` or `dmabuf`, plus `iov_count, access, offset` (must be 0), `requested_key, context, auth_key*, iface`, a `device` union (`cuda, ze, neuron, synapseai, rocr`), `hmem_data` (must be NULL), `page_size`, `base_mr` (sub-MR), and `sub_mr_cnt` [verified] `fi_mr.3`.
- **`enum fi_hmem_iface`** has `FI_HMEM_SYSTEM, FI_HMEM_CUDA, FI_HMEM_ROCR, FI_HMEM_ZE, FI_HMEM_NEURON, FI_HMEM_SYNAPSEAI`. `iface` "is ignored unless the application has requested the FI_HMEM capability" [verified] `fi_mr.3`.
- **dmabuf.** `struct fi_mr_dmabuf { int fd; uint64_t offset; size_t len; void *base_addr; }` is selected by the `FI_MR_DMABUF` flag and is "only usable for domains opened with FI_HMEM" [verified] `fi_mr.3`. `fabric.h` has `FI_MR_DMABUF` commented out at `1ULL << 40`, so it is presumably defined in another header now [inferred].
- **`mr_mode` bits** (domain attribute; the app declares which restrictions it can live with) [verified] `fi_domain.3`, `fi_mr.3`:
  - `FI_MR_LOCAL`: even local buffers must be registered and the `desc` passed.
  - `FI_MR_VIRT_ADDR`: remote peers address by VA, not by offset.
  - `FI_MR_ALLOCATED`: pages must be backed at registration, and the app must not `free()` or remap them.
  - `FI_MR_PROV_KEY`: the provider picks the key.
  - `FI_MR_MMU_NOTIFY`: the app must call `fi_mr_refresh` when mappings change.
  - `FI_MR_RMA_EVENT`, `FI_MR_ENDPOINT`: the MR is created disabled and must be bound and then enabled with `fi_mr_enable`.
  - `FI_MR_HMEM`: device buffers must be registered through `fi_mr_regattr` with `iface`/`device` filled in.
  - `FI_MR_RAW`, `FI_MR_COLLECTIVE`.
  - `FI_MR_BASIC`, `FI_MR_SCALABLE`, `FI_MR_UNSPEC`: deprecated; "Deprecate old MR modes" [verified] `NEWS.md`.
- **`desc` may be NULL** when `FI_MR_LOCAL`/`FI_MR_HMEM` are not required: "In libfabric 1.22 and later, the desc parameter must be either valid or NULL. If the desc parameter is NULL, any required local memory registration will be handled by the provider" [verified] `fi_mr.3`.
- **Lifetime rules** [verified] `fi_mr.3`:
  - Ops referencing an MR being closed "may attempt to access an invalid memory region and fail".
  - New ops against a closed MR fail.
  - "Applications are responsible for ensuring that a MR is no longer needed prior to closing it."
  - Endpoint-bound MRs must be closed before the endpoint; otherwise the close returns `-FI_EBUSY`.
  - A base MR cannot be closed while sub-MRs exist.
  - **There is no refcount protection against in-flight local ops.**
- **Cost and caching.** "The performance impact of registering memory regions can be significant." A registration cache is controlled by `FI_MR_CACHE_MAX_SIZE`, `FI_MR_CACHE_MAX_COUNT` (0 disables it), and `FI_MR_CACHE_MONITOR` (`userfaultfd, memhooks, kdreg2, disabled`) [verified] `fi_mr.3`. To opt into the cache, "simply not setting the relevant mr_mode bits" [verified] `fi_setup.7`.

**MTL relevance.**

- MTL today exposes `mtl_dma_map(mt, vaddr, size) -> iova`, `mtl_dma_unmap(mt, vaddr, iova, size)`, `mtl_dma_mem_alloc/free/addr`, `mtl_hp_malloc`, and per-frame `st20_ext_frame` with an explicit IOVA [verified] `mtl_api.h`, `st20_api.h`. There is also `ST20P_RX_FLAG_USE_GPU_DIRECT_FRAMEBUFFERS` [verified] `st_pipeline_api.h`.
- A libfabric-shaped `mtl_mr_reg(attr{iov, iface, device, dmabuf}) -> mtl_mr handle` would unify these. Frames would then reference `{mr, offset}` or a `desc` instead of raw IOVA, and the same struct would cover system, CUDA, Level Zero and dmabuf memory [inferred].
- MTL should be *stricter* than libfabric on lifetime. With one implementation, per-MR refcounting of in-flight frames is cheap, so `mtl_mr_close()` can return `-EBUSY` instead of leaving it undefined [inferred].
- The implicit-registration (`desc == NULL`) plus cache model maps to "the library copies or registers for you" (non-ext-frame mode) [inferred].

---

## 4. Data-transfer operations

```c
ssize_t fi_send(struct fid_ep *ep, const void *buf, size_t len, void *desc, fi_addr_t dest, void *context);
ssize_t fi_sendv(struct fid_ep *ep, const struct iovec *iov, void **desc, size_t count, fi_addr_t dest, void *context);
ssize_t fi_sendmsg(struct fid_ep *ep, const struct fi_msg *msg, uint64_t flags);
ssize_t fi_inject(struct fid_ep *ep, const void *buf, size_t len, fi_addr_t dest);
struct fi_msg { const struct iovec *msg_iov; void **desc; size_t iov_count; fi_addr_t addr; void *context; uint64_t data; };
```

[verified] `fi_msg.3`. The simple calls use the endpoint's default `op_flags`; `*msg` variants take explicit flags [verified] `fi_endpoint.3`, `fi_msg.3`.

### 4.1 Flags (bit values from `fabric.h`)

| Flag | Semantics | Source |
|---|---|---|
| `FI_COMPLETION` (1<<24) | generate a completion for this op; only meaningful when the endpoint is bound with `FI_SELECTIVE_COMPLETION`, "or this flag is ignored" | [verified] `fi_msg.3` |
| `FI_MORE` (1<<18) | "the user has additional requests that will immediately be posted after the current call returns" — a doorbell-batching hint. Providers that delay "must ensure that all previously delayed calls be flushed when an error is returned from a new call" | [verified] `fi_msg.3` |
| `FI_INJECT` (1<<25) | buffer "returned to user immediately after the send call returns"; the provider may copy; only for sizes up to `inject_size`. `fi_inject()` also suppresses the success completion, but failures still produce an error CQ entry | [verified] `fi_msg.3` |
| `FI_INJECT_COMPLETE` (1<<26) | completion "when the source buffer(s) may be reused"; "does not indicate that the data has been transmitted onto the network" | [verified] `fi_msg.3`, `fi_cq.3` |
| `FI_TRANSMIT_COMPLETE` (1<<27) | "completed relative to the local provider". Reliable endpoints: "delivered to the peer endpoint". **Unreliable endpoints: "delivered to the fabric" ... "no longer dependent on local resources"** | [verified] `fi_cq.3` |
| `FI_DELIVERY_COMPLETE` (1<<28) | "processed by the destination endpoint(s)"; "applies only to reliable endpoints" | [verified] `fi_cq.3` |
| `FI_MATCH_COMPLETE`, `FI_COMMIT_COMPLETE` | matched to a posted buffer; persisted (experimental) | [verified] `fi_cq.3` |
| `FI_FENCE` | defer until prior ops to the same peer complete | [verified] `fi_msg.3` |
| `FI_MULTICAST` | destination is a multicast `fi_addr_t` from `fi_mc_addr()` | [verified] `fi_msg.3`, `fi_cm.3` |
| `FI_MULTI_RECV` | one posted buffer receives many messages; a final completion with `FI_MULTI_RECV` fires when it is consumed, "even under selective completion" | [verified] `fi_msg.3`, `fi_cq.3` |

"In all cases, a completion indicates that it is safe to reuse the buffer(s)" [verified] `fi_setup.7`. The completion levels therefore only strengthen *what else* is guaranteed beyond reuse. **None of the man pages consulted states a single default completion level** when no flag is set. `fi_setup.7` recommends "the provider's default flags for best performance"
[verified: absence of a default in `fi_endpoint.3`/`fi_cq.3`; the recommendation is in `fi_setup.7`].

### 4.2 Context, iov/desc, backpressure

- **`context`.** "User specified pointer to associate with the operation", returned in the completion. It is ignored if no success completion will be generated [verified] `fi_msg.3`. Under `FI_CONTEXT`, the pointer must be a provider-writable `struct fi_context { void *internal[4]; }` that stays valid until completion or cancel [verified] `fabric.h`, `fi_getinfo.3`.
  The wiki warns that misuse "might appear as stack corruption which can be hard to debug!" [verified] "Common OFI Mistakes".
- **iov/desc arrays.** Scatter-gather comes with a parallel `desc[]` per segment, capped by `tx_attr.iov_limit`. `FI_ASYNC_IOV` mode forbids touching the iovec array itself until completion [verified] `fi_msg.3`, `fi_endpoint.3`, `fi_getinfo.3`.
- **`-FI_EAGAIN`.** The provider "currently lacks the resources needed to initiate the requested operation"; the app may retry later [verified] `fi_msg.3`.
  - Crucially: "When using FI_PROGRESS_MANUAL, the application must check for transmit and receive completions after receiving FI_EAGAIN ... independent of the operation which failed. This is also strongly recommended when using FI_PROGRESS_AUTO" [verified] `fi_msg.3`.
  - Queue depth is `tx_attr.size`, but "there is not necessarily a one-to-one mapping between a transmit operation and a queue entry" [verified] `fi_endpoint.3`.

**MTL relevance.**

- The MTL pipeline `st20p_tx_get_frame()` returns a frame or NULL, and `put_frame()` submits it. The session API instead *pulls* via `get_next_frame(priv, &idx, meta)` callbacks from the lcore [verified] `st20_api.h`, `st_pipeline_api.h`. Libfabric is purely **push/post**; MTL's session API is **pull**. A unified API should choose one, and push fits with libfabric and Rivermax [inferred].
- Mapping `-EAGAIN` for "no free frame slot / TX ring full" onto an errno-style return is a direct win over NULL [inferred].
- `FI_INJECT` fits ST 2110-40/-41 and -30 well. Those units are tiny, 1 to 8 packets per frame [verified] repository `CLAUDE.md`, so "copy on submit, reusable on return" removes a whole lifecycle for them [inferred].
- `FI_MORE` maps to batching several audio or ancillary units into one burst [inferred].

---

## 5. Completion queues, wait objects, counters

### 5.1 CQ read and formats

- **Entry formats** [verified] `fi_cq.3`. Each is a prefix-compatible struct:
  - `FI_CQ_FORMAT_CONTEXT`: `{op_context}`.
  - `MSG`: adds `flags, len`.
  - `DATA`: adds `buf, data`.
  - `TAGGED`: adds `tag`.
- **Read calls.** `fi_cq_read(cq, buf, count)` is non-blocking, batched, and returns a count or `-FI_EAGAIN`. `fi_cq_sread(cq, buf, count, cond, timeout_ms)` blocks: "A negative value indicates infinite timeout", and it returns early if signalled (`fi_cq_signal`) [verified] `fi_cq.3`.
- **Reads drive progress.** "A count value of 0 may be used to drive progress on associated endpoints when manual progress is enabled" [verified] `fi_cq.3`.
- **Errors are out of band.**
  - A failed op goes to an error queue. `fi_cq_read` then returns `-FI_EAVAIL`, and the app must call `fi_cq_readerr` before normal entries resume [verified] `fi_cq.3`, `fi_setup.7`.
  - `struct fi_cq_err_entry` has `op_context, flags, len, buf, data, tag, olen, err, prov_errno, err_data, err_data_size, src_addr` [verified] `fi_cq.3`.
  - `err` is "a positive fabric errno". `prov_errno` is provider specific and "intended to be used as a debugging aid". `err_data` is a caller-supplied buffer that is filled up to `err_data_size`; legacy mode instead returns a provider buffer valid until the next read [verified] `fi_cq.3`.
  - `fi_cq_strerror(cq, prov_errno, err_data, buf, len)` prints them [verified] `fi_eq.3` (the `fi_cq` analogue has the same shape [inferred]).
- **Error codes** are `errno` values (`FI_EAGAIN = EAGAIN`, ...) plus fabric-specific codes from 256: `FI_EAVAIL, FI_ETRUNC, FI_EOVERRUN, FI_ENORX, FI_ENOCQ, FI_ENOEQ, ...` [verified] `fi_errno.h`.
- **Ordering.** Completions may be out of order: "Handling out of order completions can increase application complexity, but it does allow for optimizing network utilization" [verified] `fi_intro.7`.

### 5.2 Wait objects

- **Types** [verified] `fi_cq.3`, `fi_eq.3`, `fi_poll.3`:
  - `FI_WAIT_NONE` (default; `sread` is not allowed).
  - `FI_WAIT_UNSPEC` (provider chooses; the app "not guaranteed to retrieve the underlying wait object").
  - `FI_WAIT_FD` ("must be usable in select, poll, and epoll").
  - `FI_WAIT_YIELD` (spin).
  - `FI_WAIT_SET` and `FI_WAIT_MUTEX_COND` (deprecated).
- **Getting the native object.** `fi_control(cq, FI_GETWAIT, &fd)` returns it [verified] `fi_cq.3`.
- **`fi_trywait` closes the check-then-block race.** "The application must call fi_trywait and obtain a return value of FI_SUCCESS prior to blocking on a native wait object. Failure to do so may result in the wait object not being signaled." `-FI_EAGAIN` means events are queued, so drain and retry [verified] `fi_poll.3`. It is **not** deprecated [verified] `fi_poll.3`.
- **`wait_cond = FI_CQ_COND_THRESHOLD`** is only a hint: "Providers are not required to meet the requirements of the condition before signaling" [verified] `fi_cq.3`.
- **`FI_AFFINITY` plus `signaling_vector`** is a CPU hint for interrupts [verified] `fi_cq.3`.

### 5.3 Selective completion

- **Binding.** Bind with `FI_TRANSMIT|FI_SELECTIVE_COMPLETION`. Success entries then appear only for ops flagged `FI_COMPLETION` [verified] `fi_endpoint.3`.
- **Errors are never suppressed.** "Operations that fail asynchronously will still generate completions, even if a completion is not requested" [verified] `fi_endpoint.3`. Unknown fields such as `op_context` come back NULL/0 [verified] `fi_cq.3`.
- **A CQ is required even when all successes are suppressed.** "All endpoints that issue asynchronous operations must be bound to a relevant CQ, even if they don't report completions" [verified] wiki "Common OFI Mistakes", `fi_endpoint.3`.

### 5.4 CQ size and overrun — can completions be lost?

- **Size.** `size` is "the minimum size of a completion queue", and 0 means provider default [verified] `fi_cq.3`.
- **Overrun is fatal.** An overrun CQ keeps returning "valid, non-corrupted completions", then returns `FI_EOVERRUN`; "Overrun completion queues are considered fatal" [verified] `fi_cq.3`. **So yes — completions can be lost, and the CQ is then unusable.**
- **`FI_RM_ENABLED`.** The provider protects the CQ: it may fail the post with `-FI_EAGAIN` or retry internally [verified] `fi_domain.3`.
  - With selective completion, "CQ space for failures is not reserved", so the app should size the CQ at least as large as the bound queues [verified] `fi_domain.3`.
- **`FI_RM_DISABLED`.** CQ overrun causes "an undefined, but fatal, error ... affecting all endpoints associated with the CQ" [verified] `fi_domain.3`.

### 5.5 Counters

- **Calls** [verified] `fi_cntr.3`:
  - `fi_cntr_read`, `fi_cntr_readerr`, `fi_cntr_add/set/adderr/seterr`.
  - `fi_cntr_wait(cntr, threshold, timeout_ms)`: it returns once the value is at least the threshold, `-FI_ETIMEDOUT` on timeout, and "Any change in a counter's error value will unblock any thread inside fi_cntr_wait".
- **Events.** `FI_CNTR_EVENTS_COMP` counts completions; `FI_CNTR_EVENTS_BYTES` counts bytes [verified] `fi_cntr.3`.
- **Counters work with selective completion.** "Counters increment on all successful completions, separately from whether the operation generates an entry" [verified] `fi_cntr.3`. They are "a light-weight completion mechanism", for example for credit-based flow control [verified] `fi_cntr.3`.
- **Visibility caveat.** "A small, but undefined, delay may occur between the counter changing and the reported value being updated" [verified] `fi_cntr.3`.

**MTL relevance.**

- **Completions today are callbacks from the lcore.** MTL uses `notify_frame_done(priv, idx, meta)`, `notify_frame_available`, and `notify_frame_ready`, with the warning "only non-block method can be used within this callback as it run from lcore" [verified] `st_pipeline_api.h`, `st20_api.h`.
  Libfabric deliberately chose **app-polled queues**: a CQ "gives the application control over when and how to process completed requests" [verified] `fi_intro.7`. Callbacks appear only in the provider-to-provider peer API (§6.2).
- **Proposal.** Make the queue primary: `mtl_cq_read(cq, entries[], n)`, with `{user_ctx, status, flags, timestamps}` per entry. Keep callbacks as an optional, clearly data-plane-restricted adapter [inferred].
- **Overrun can be ruled out by construction.** An MTL per-session CQ can be sized equal to the session's frame count (`framebuff_cnt`) — the maximum in flight — which makes libfabric's fatal-overrun case impossible [inferred].
- **Do not copy the error queue for media quality.** MTL RX frames carry `enum st_frame_status { COMPLETE, RECONSTRUCTED, CORRUPTED, DROPPED }` [verified] `st_api.h`. A corrupted frame is still *data plus a quality verdict*, not an error. Routing it through a separate `-EAVAIL`/`readerr` path would be wrong. Put status inline in every entry, and reserve an error path for true failures [inferred].
- **Wait support.** `FI_WAIT_FD` plus `fi_trywait` is exactly what GStreamer, FFmpeg and OBS plugins need for epoll integration. It is better than the current `*_FLAG_BLOCK_GET` with `set_block_timeout` (default 1 s) [verified flags exist in `st_pipeline_api.h`; benefit inferred].
- **Counters.** Counters are the natural shape for MTL stats-as-completion, e.g. "frames transmitted", "late", "dropped". `fi_cntr_wait(threshold)` maps to "wait until N frames done" [inferred].

---

## 6. Event queue (control plane)

- **Scope.** "Event queues are used to report events associated with control operations ... memory registration, address vectors, connection management, and fabric and domain level events", plus asynchronous errors [verified] `fi_eq.3`. Separating EQs from CQs is deliberate "for performance reasons", and EQs are often implemented in software [verified] `fi_arch.7`.
- **Events** [verified] `fi_eq.3`:
  - `FI_CONNREQ`, `FI_CONNECTED`, `FI_SHUTDOWN` via `struct fi_eq_cm_entry { fid; info; data[]; }`.
  - `FI_MR_COMPLETE`, `FI_AV_COMPLETE`, `FI_JOIN_COMPLETE` via `struct fi_eq_entry { fid; context; data; }`.
  - `FI_NOTIFY` is **not** on the current `fi_eq(3)` page [verified absence]. It exists in `fi_eq.h` as an event ID [inferred].
  - Async MR registration and async AV insert are deprecated in 2.0 [verified] `NEWS.md`, `fi_av.3`.
- **Read calls** [verified] `fi_eq.3`:
  - `fi_eq_read(eq, &event, buf, len, flags)` returns "At most one event" per read, and supports `FI_PEEK`.
  - `fi_eq_readerr` works as for CQs.
  - `fi_eq_sread(... timeout)`.
- **User-inserted events.** `fi_eq_write` is allowed only if the EQ was opened with `FI_WRITE` [verified] `fi_eq.3`. This lets an app funnel its own control events into the same loop.
- **Overrun is fatal**, as for CQs [verified] `fi_eq.3`.
- **Multicast join is asynchronous.** Results come via `FI_JOIN_COMPLETE`: "Applications cannot issue multicast transfers until receiving notification that the join operation has completed" [verified] `fi_cm.3`.

**MTL relevance.** Today `notify_event(priv, enum st_event, args)` carries `ST_EVENT_VSYNC`, `ST_EVENT_RECOVERY_ERROR` and `ST_EVENT_FATAL_ERROR` [verified] `st_api.h`, and `notify_frame_late`/`notify_detected`/RTCP arrive through separate callbacks [verified] `st20_api.h`. An EQ-shaped per-instance (and optionally per-session) event queue would carry these control events [inferred]:

- PTP lock and unlock.
- Link up and down.
- Recovery started and done.
- Format auto-detected.
- IGMP join complete.
- Fatal session error.
- MtlManager lost.

Whether VSYNC belongs in the EQ or in the data CQ is an open question (Open question 11). It is time-critical, while EQs are "not a fast path" [verified] a maintainer's remark in libfabric PR #5566: "Reading the EQ isn't supposed to be a fast path operation".

### 6.2 Peer API (for completeness)

`fi_peer(3)` lets one provider write completions into a CQ owned by another provider through `owner_ops->write/writeerr`. "The owner is responsible for locking, event signaling, and handling CQ overflow" [verified]. The peer APIs "are developmental and may change" and "are not designed for direct use by applications" [verified] `fi_peer.3`, `fi_provider.7`. `fi_import_fid`
"may be used to import a fabric object created and owned by the libfabric user", but its details are "outside the scope" [verified] `fi_peer.3`. **App-supplied CQ sinks are therefore not a stable libfabric concept.** MTL could still offer one, e.g. "deliver completions to my ring or my callback", for the FFmpeg/GStreamer glue [inferred].

---

## 7. Progress, threading, resource management

### 7.1 Progress

`fi_domain_attr` has `control_progress` and a union `{data_progress, progress}` [verified] `fabric.h`. The 2.0 release "Simplify progress definition" [verified] `NEWS.md`. The man page now describes one `progress` field governing both [verified] `fi_domain.3`.

| Model | Promise | Source |
|---|---|---|
| `FI_PROGRESS_AUTO` | "the provider will make forward progress on an asynchronous operation without further intervention by the application". Forcing AUTO on a provider without native support "may create hidden threads" | [verified] `fi_domain.3` |
| `FI_PROGRESS_MANUAL` | "the provider requires the use of an application thread to complete an asynchronous request". Progress happens only inside fabric calls that read or wait on CQ/EQ/counter; "OS calls like select/poll do not count" | [verified] `fi_domain.3` |
| `FI_PROGRESS_CONTROL_UNIFIED` | "the user will synchronize progressing the data and control operations themselves"; implies manual. Combined with DOMAIN or COMPLETION threading, it "allows Libfabric to remove all locking in the critical data progress path" | [verified] `fi_domain.3` |

Rationale: internal progress threads cost measurably, and "The manual progress model can avoid this overhead", but "it is critical that the application thread call into libfabric in a timely manner" [verified] `fi_setup.7`.

### 7.2 Threading — what the *app* promises

| Model | App promise | Source |
|---|---|---|
| `FI_THREAD_SAFE` | none; "All providers are required to support" it | [verified] `fi_domain.3` |
| `FI_THREAD_DOMAIN` | "serialize access to all objects under the same domain" | [verified] |
| `FI_THREAD_COMPLETION` | serialize access to all objects sharing a completion mechanism; one thread per CQ/context | [verified] |
| `FI_THREAD_ENDPOINT`, `FI_THREAD_FID` | per-endpoint or per-object serialization; **deprecated** ("Simplify threading models" in 2.0) | [verified] `fi_domain.3`, `NEWS.md` |

"Control interfaces are always considered thread safe unless the control progress model is FI_PROGRESS_CONTROL_UNIFIED" [verified] `fi_domain.3`. `fi_setup.7` recommends targeting `FI_THREAD_SAFE` or `FI_THREAD_DOMAIN`, and warns "providers may still implement more serialization than is needed" [verified].

### 7.3 Resource management

- **`FI_RM_ENABLED`.** A full TX/RX queue fails the post with `EAGAIN`; CQ overrun risk is handled by failing the post or retrying internally; missing RX buffers on reliable endpoints are retried internally [verified] `fi_domain.3`.
- **`FI_RM_DISABLED`.** "The application is responsible for resource protection." Queue overrun is "fatal to the context", CQ overrun is fatal to every endpoint on the CQ, and endpoints get disabled and must be re-enabled [verified] `fi_domain.3`.

**MTL relevance.**

- MTL is essentially `FI_PROGRESS_AUTO` with visible, pinned progress threads: scheduler lcores running tasklets, or pthreads with `MTL_FLAG_TASKLET_THREAD` [verified] `mtl_api.h`, repository `CLAUDE.md`. `mtl_sch_create/register_tasklet` already lets apps host tasklets [verified] `mtl_sch_api.h`.
- A `MANUAL`/`CONTROL_UNIFIED`-like mode — the app thread calls `mtl_progress(session)` — is conceivable for RX and for software-paced TX. It is **not** safe for hardware-paced ST 2110-20 at 4500 packets per frame with microsecond pacing, where a late app thread breaks compliance [inferred].
- The threading promise worth copying is `FI_THREAD_COMPLETION`: one thread per session CQ, lock-free. It pairs with MTL's rule that data-plane callers never take mutexes [inferred].
- Whether current `st20p_*_get/put_frame` are safe to call concurrently from multiple app threads is **[unknown]**; not checked.
- MTL should be `FI_RM_ENABLED` only: always `-EAGAIN`, never fatal overrun [inferred].

---

## 8. Cancel, close, flush

- **`fi_cancel(fid, context)`** [verified] `fi_endpoint.3`:
  - It is asynchronous and "will complete within a bounded period of time".
  - A cancelled op produces an error entry with `FI_ECANCELED`, and "No specific entry related to fi_cancel itself will be posted".
  - It is not guaranteed: if the op already completed, the normal completion appears instead.
  - Ops without a valid context "cannot be canceled"; if several ops match a context, one is cancelled and which one is provider-defined.
  - `-FI_EAGAIN` means retry after progress.
- **`fi_close(ep)` with outstanding ops.** "Discarded operations will silently be dropped, with no completions reported." Buffers must not be reused until a completion arrives or `fi_close` returns, and the provider may also discard already-queued CQ entries [verified] `fi_endpoint.3`.
- **Critical errors.** "When an endpoint is disabled as a result of a critical error, all pending operations are discarded" [verified] `fi_endpoint.3`.
- **There is no `fi_ep_flush`.** Nothing in `fi_endpoint.3` flushes outstanding ops into error completions [verified absence]. The only flush is `FI_FLUSH_WORK` for deferred work queues in `fi_trigger.3` [verified].
- **Ordering of closes.** Closing a scalable endpoint with open contexts returns `-FI_EBUSY`; MRs bound to an endpoint must be closed first [verified] `fi_endpoint.3`, `fi_mr.3`.

**MTL relevance.** Silent discard is wrong for MTL. Ext-frame users (`ST20P_TX_FLAG_EXT_FRAME`, `..._MANUAL_RELEASE` with `st20p_tx_notify_ext_frame_free()` [verified] `st_pipeline_api.h`) need to reclaim every buffer they lent, often GPU or hugepage memory. Recommended contract [inferred]:

1. `mtl_session_stop()` or `flush()` stops scheduling new units.
2. Every outstanding user-owned buffer is returned as a completion with `status = CANCELED`.
3. After destroy returns, no further completions or callbacks fire.

This is the libfabric `fi_cancel` semantic applied to everything, with the guarantee libfabric lacks.

---

## 9. ABI and versioning

- **Version encoding.** `FI_VERSION(major, minor) = (major << 16) | minor`; `fi_version()` returns the library version [verified] `fabric.h`, `fi_version.3`.
- **The app states the version it was written for.** "Applications should use the FI_VERSION(major, minor) macro to indicate the version, with hard-coded integer values"; passing `FI_MAJOR_VERSION`/`FI_MINOR_VERSION` risks uninitialized new fields on rebuild [verified] `fi_getinfo.3`.
  "the version parameter allows libfabric to determine if an application is aware of new fields that may have been added to structures" [verified] `fi_setup.7`. A version newer than the library returns `-FI_ENOSYS` [verified] `fi_getinfo.3`.
- **Two version numbers.** The API version is `FI_MAJOR/MINOR_VERSION`; the ABI version is the symbol version in `libfabric.map`. Runtime rule: `(va1 <= va2 <= va3) && (vb1 <= vb2 <= vb3)` from app to provider to core [verified] wiki "ABI Versions".
- **Mechanism** [verified] `src/abi_1_0.c`, `fabric.7`:
  - Structs grow by appending only.
  - Frozen copies of old layouts (`struct fi_info_1_0`, `fi_domain_attr_1_0`, ...) are kept.
  - Per-version shims `COMPAT_SYMVER(fi_getinfo_1_0, fi_getinfo, FABRIC_1.0)` exist for `FABRIC_1.0/1.1/1.2/1.3/1.7/1.8/1.9`.
  - Shims convert old hints up ("Any new fields in the latest definition will be zeroed") and cast results back.
- **ABI history** [verified] `fabric.7`:

  | ABI | libfabric | Change |
  |---|---|---|
  | 1.1 | 1.5 | `api_version`, `mr_mode` enum → bitfield, many `domain_attr` fields |
  | 1.2 | 1.7 | `fi_info.nic` |
  | 1.3 | 1.9 | `tclass` |
  | 1.4 | 1.12 | `fi_tostr_r` |
  | 1.5 | 1.13 | `fi_open` |
  | 1.6 | 1.14 | `fi_log_ready` |
  | 1.7 | 1.20 | `max_ep_auth_key` |
  | 1.8 | 2.0 | `fi_fabric2`, `max_group_id` |
  | 1.9 | 2.5 | `max_cntr_value` |

  Libfabric 2.0 was a major API version but kept ABI compatibility ("1.8 ABI compat") [verified] `NEWS.md`.
- **Enums whose values are baked in never change.** Comments in `fabric.h` note `FI_ATOMIC_OP_LAST` and `FI_DATATYPE_LAST` "cannot change", deprecated provider IDs are kept "to save binary compatibility", and removed flags remain as zero-valued stubs [verified] `fabric.h`. Deprecations use `_Pragma("GCC warning ...")` on the macro, e.g. `FI_LOCAL_MR` and `FI_MR_BASIC` [verified] `fabric.h`.
- **Only lib-allocated `fi_info`.** "Applications must use libfabric allocated fi_info structures" [verified] `fi_getinfo.3`. The library therefore always knows the true allocation size.

**MTL relevance.**

- MTL has `MTL_VERSION_NUM(a,b,c)` and `mtl_version()` returns a string [verified] `mtl_api.h`.
- `mtl_init(struct mtl_init_params*)` and every `*_ops` struct are app-allocated with no size or version field, and no `.map` file was found under `lib/` [verified via `ls`/`grep`]. Any field added to `mtl_init_params` or `st20p_tx_ops` is therefore an ABI break [inferred].
- Cheapest libfabric-derived fixes, in order [inferred]:
  1. Pass a hard-coded API version into `mtl_init`, e.g. `mtl_init2(MTL_API_VERSION(2,0), &p)`, or put `uint32_t struct_size` as the first member of every ops/params struct.
  2. Keep all objects opaque handles.
  3. Add a linker version script only if multi-version shims are actually needed.
- Libfabric's `abi_1_0.c` shows the ongoing cost of shims [inferred].

---

## 10. Criticisms and pain points — what MTL should NOT copy

| Pain point | Evidence | Lesson for MTL |
|---|---|---|
| **Combinatorial negotiation (caps × modes × mr_mode × attrs).** Portable apps must handle every mode bit a provider might leave set | Mode semantics [verified] `fi_getinfo.3`; the 2.0 cleanup removed or deprecated several modes and MR modes [verified] `NEWS.md` | One implementation: no mode bits. Report granted values; never ask the app to adapt its buffer layout |
| **Too many threading and progress models,** later collapsed | `FI_THREAD_FID`/`ENDPOINT` deprecated; "Simplify threading models", "Simplify progress definition" [verified] `fi_domain.3`, `NEWS.md` | Offer at most two threading promises (thread-safe; single thread per session) and one progress model (library-owned) plus possibly an explicit app-driven variant |
| **Wait sets and poll sets** added, then deprecated | [verified] `fi_poll.3`, `NEWS.md` | Provide an FD per queue and `trywait`; let the app use epoll. Do not build an aggregation object |
| **Provider inconsistency.** Only RDM endpoints and basic send/recv are universal; `FI_MULTICAST` is supported (limited) by `udp` alone; `efa` and `efa-direct` differ widely | [verified] wiki "Provider Feature Matrix v2.7.x" | MTL backends (DPDK PMD, AF_XDP, kernel socket, RDMA) will differ too. Capability query per port/backend is mandatory, and fallback must be *reported*, not silent |
| **Fatal CQ/EQ overrun; `RM_DISABLED` makes errors fatal to the context** | [verified] `fi_cq.3`, `fi_domain.3` | Size queues by construction (≥ frames in flight); always backpressure with `-EAGAIN` |
| **Silent discard on close** | [verified] `fi_endpoint.3` | Flush every user buffer back with a `CANCELED` status |
| **`FI_CONTEXT` scratch owned by the provider but allocated by the app** — stack-corruption footgun | [verified] wiki "Common OFI Mistakes" | `user_ctx` is an opaque cookie only; the library keeps its own per-frame state |
| **MR close with in-flight ops is undefined** | [verified] `fi_mr.3` | Refcount and return `-EBUSY` |
| **Error queue separate from success queue (`-FI_EAVAIL` gate)** forces two read paths and blocks normal entries until the error is read | [verified] `fi_cq.3` | Inline status per entry; media quality is not an error |
| **No default completion level stated** in the man pages consulted; the app is told to trust provider defaults | [verified absence] `fi_endpoint.3`, `fi_cq.3`; `fi_setup.7` recommends defaults | Define the single default explicitly, e.g. "buffer released" |
| **Generality tax in the fast path.** Every valid flag "may need a separate check, resulting in potentially dozens of checks" (libfabric's own words about sockets, applied to its `*msg` calls) | [verified] `fi_intro.7` | Fixed per-session defaults set at create (libfabric's `op_flags`); the fast path takes no flags or very few |
| **Very large surface for a narrow need** (RMA, atomics, tagged, collectives, AV sets, triggers) | [inferred] from the man page set | MTL's domain is one-way isochronous streams: keep the object set minimal |

---

## 11. Mapping table: libfabric concept → proposed MTL analogue

| libfabric | Proposed MTL analogue | Fit | Where the analogy breaks |
|---|---|---|---|
| `fi_getinfo` + hints → `fi_info` list | `mtl_query_caps(port or backend)` → caps struct; plus `requested → granted` attrs returned from session create | Good | MTL has no provider list. Discovery is per port/backend (PMD vs AF_XDP vs kernel) and per feature (HW pacing, PTP, DMA, header split, ST 2022-7) |
| caps (top-down) | session feature flags, required vs optional | Good | — |
| mode bits (bottom-up) | **none** | Do not copy | Single implementer |
| `fid_fabric` | process-wide library init (`mtl_init`) | Partial | libfabric fabrics can span providers; MTL has one instance per process (DPDK EAL) |
| `fid_domain` | per-port/NIC context inside the instance, or the instance itself | Partial | MTL sessions span *two* ports for ST 2022-7; a domain is one NIC |
| `fid_ep` (`FI_EP_DGRAM` + `FI_MULTICAST`) | `mtl_session` (TX or RX, per media type) | Partial | An MTL session is a *stream with a clock* (epoch, fps, RTP timestamp), not a message pipe. There is no peer, connection or ACK |
| `fi_enable` after bind | `mtl_session_start` after create/configure | Good | MTL also has instance-level `mtl_start` [verified] `mtl_api.h` |
| `fid_av`, `fi_addr_t` | destination/source flow descriptor (IP, port, mcast, SSRC, payload type) set at create | Weak | MTL sessions have 1–2 fixed destinations; no fast-path per-op address |
| `fi_join` / `fid_mc` / `FI_JOIN_COMPLETE` | IGMP join as part of RX session start; completion as EQ event | Good | — |
| `fid_mr`, `fi_mr_regattr{iov, iface, device, dmabuf}`, `fi_mr_desc` | `mtl_mr_reg(attr{iov, mem_type SYSTEM/HUGEPAGE/CUDA/ZE/DMABUF, device})` → handle; frames reference `{mr, offset}` | Good | Must add refcounted close (`-EBUSY`). Registration should also cover *frame pools*, not just arbitrary buffers |
| `desc == NULL` → provider registers internally, MR cache | "library-owned frame" mode (current non-ext-frame pipeline) | Good | — |
| `context` (`void*`) | `user_ctx` in every submitted unit, returned verbatim in its completion | Good | Maps to today's `st_frame.opaque` / `st20_ext_frame.opaque` [verified] `st_pipeline_api.h`, `st20_api.h` |
| `fi_send` / `fi_sendmsg` + `iov[]` + `desc[]` | `mtl_tx_submit(session, unit{iov[], mr, user_ctx, timestamp, flags})` | Good | A unit has a presentation time and deadline; libfabric ops have neither |
| `FI_INJECT`, `inject_size` | copy-on-submit for ST 2110-40/-41 (and optionally -30): buffer reusable at return | Good | — |
| `FI_MORE` | batch hint for multiple small units | Minor | — |
| `FI_INJECT_COMPLETE` | completion level `BUFFER_RELEASED` (all packets built or copied; app may reuse) | Good | In zero-copy ext-frame mode, this equals "NIC finished DMA-reading", i.e. transmit-complete [inferred] |
| `FI_TRANSMIT_COMPLETE` (unreliable: "delivered to the fabric") | completion level `ON_WIRE` with **actual TX time** (PTP or HW timestamp of first/last packet) | Partial | libfabric reports no time. MTL needs "went out at T vs scheduled T0" for compliance and late detection |
| `FI_DELIVERY_COMPLETE` | **none** for multicast UDP; possibly RTCP-based retransmit feedback | None | Only defined for reliable endpoints [verified] `fi_cq.3` |
| `FI_TRIGGER` (counter threshold) | **time trigger**: "send at epoch / PTP time T" (today's `ST20_TX_FLAG_USER_PACING`/`USER_TIMESTAMP`) | None in libfabric | libfabric has no time-based trigger [verified] `fi_trigger.3` |
| `fid_cq` + `fi_cq_read` batched | per-session completion queue: `mtl_cq_read(cq, entries[], n)`; one CQ may be shared by many sessions | Good | Entries need media fields: status (complete, reconstructed, corrupted, dropped, late, canceled), RTP timestamp, PTP timestamps, per-port packet counts |
| `fi_cq_err_entry` / `-FI_EAVAIL` | inline `status` + `errno` in every entry; optional `err_data` for diagnostics | Change | Media degradation is not an error |
| `FI_SELECTIVE_COMPLETION` + errors always reported | per-session "completions only for flagged units", errors always | Good | — |
| CQ `size` / overrun | CQ depth ≥ max in-flight units, enforced at create | Better than libfabric | — |
| `FI_WAIT_FD`, `FI_GETWAIT`, `fi_trywait`, `fi_cq_sread(timeout)` | `mtl_cq_get_fd()` (eventfd), `mtl_cq_trywait()`, `mtl_cq_wait(timeout)` | Good | Waking an app thread must never be done from a tasklet with a syscall on the hot path; requires a design such as armed-only signalling [inferred] |
| `fid_eq` (`CONNREQ/CONNECTED/SHUTDOWN/MR_COMPLETE/JOIN_COMPLETE`), `fi_eq_write` | instance + session event queue: PTP lock/unlock, link state, recovery, format detected, IGMP join, fatal; app may inject events | Good | VSYNC/epoch tick is time-critical; the EQ is "not a fast path" |
| `fid_cntr`, `fi_cntr_wait(threshold)` | stats counters (frames sent, late, dropped, packets per port) readable lock-free; optional wait-for-N | Good | MTL stats are richer (per-port, per-redundancy-leg) than one success/error pair |
| `FI_PROGRESS_AUTO` | library-owned scheduler lcores / tasklets (current MTL) | Good | MTL progress threads are visible and pinned, not "hidden" |
| `FI_PROGRESS_MANUAL` / `CONTROL_UNIFIED` | optional app-driven `mtl_progress()` for RX or TSC-paced sessions | Risky | Hardware-paced video TX needs microsecond scheduling; a late app thread breaks ST 2110-21 |
| `FI_THREAD_SAFE` / `FI_THREAD_COMPLETION` | "session handle is thread-safe" vs "one thread per session/CQ, lock-free" | Good | — |
| `FI_RM_ENABLED` | always `-EAGAIN`, never fatal | Good | — |
| `fi_cancel` → `FI_ECANCELED` | `mtl_session_flush()` returns all user buffers with `CANCELED` | Better than libfabric | libfabric `fi_close` discards silently |
| `fi_open_ops(name)` | named, versioned extension tables for backend-specific features (DPDK queue meta, `DATA_PATH_ONLY`, AF_XDP) | Good | Keep the core API backend-neutral |
| ops table `size` + `FI_CHECK_OP` | exported functions + `struct_size` in every params/ops struct | Good | MTL does not need inline vtable dispatch |
| `FI_VERSION` passed to `fi_getinfo` | API version passed to `mtl_init` | Good | — |
| `fi_tostr` | `mtl_*_to_str()` for attrs/status (debugging) | Minor | — |

---

## 12. Other notes

- **Default `op_flags`** set once at endpoint creation avoid per-call flag parsing, and "can prepare submitted commands ahead of time" [verified] `fi_setup.7`. MTL already does the analogue by baking format, fps and pacing into the session at create. Keep that, and keep per-submit flags minimal [inferred].
- **Exposed limits.** Libfabric exposes `inject_size`, `iov_limit` and `max_msg_size` instead of hiding them [verified] `fi_endpoint.3`, `fi_setup.7`. MTL equivalents would be max frame size, max `iov` per frame, and max in-flight units [inferred].
- **Hint-only fields are ignorable.** `signaling_vector` and `FI_CQ_COND_THRESHOLD` "may be ignored" [verified]. That is a good pattern for performance hints in MTL, such as NUMA or lcore preferences [inferred].
- **Multiple frame-return paths today.** MTL's `ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE` adds a third release step after `notify_frame_done` [verified] `st_pipeline_api.h`. The accreted flags show the lack of a single completion model — exactly what a libfabric-style CQ plus defined completion levels would replace [inferred].

---

## Open questions for the maintainer

1. **Push or pull for TX?** Libfabric (and, reportedly, Rivermax) is push: the app posts a buffer and gets a completion. MTL's session API pulls (`get_next_frame` callbacks from the lcore), and the pipeline API is get/put.
   - Why it matters: it decides whether the library or the app owns "what goes out next when the epoch fires".
   - Options: (a) push only, with the library holding a per-session submit ring and a "late/empty" policy; (b) push primary plus a pull adapter for legacy apps; (c) keep both as first-class.
2. **Queue-first or callback-first completions?** Libfabric deliberately uses app-polled CQs. MTL uses lcore callbacks that must not block.
   - Why it matters: callbacks run on the data-plane thread, so app bugs break pacing, while queues cost a thread hop.
   - Options: (a) CQ only; (b) CQ plus an optional callback sink documented as data-plane context; (c) callback only, as today.
3. **Which completion levels, and which is the default?** Libfabric's inject/transmit/delivery levels answer "when can I reuse the buffer", but its man pages never pin a default.
   - Why it matters: zero-copy ext frames and GPU buffers need the exact reuse point; compliance tooling needs "actual time on wire".
   - Options: (a) one level, `BUFFER_RELEASED`, always; (b) `BUFFER_RELEASED` default plus opt-in `ON_WIRE_WITH_TIMESTAMP`; (c) both reported in one entry with two timestamps.
4. **Is media degradation a status or an error?** Libfabric sends failures to a separate error queue gated by `-FI_EAVAIL`.
   - Why it matters: RX frames are routinely `CORRUPTED`/`RECONSTRUCTED` and still useful, and late TX frames may be dropped or sent late.
   - Options: (a) inline `status` in every completion, with the error path only for session failure; (b) the libfabric split.
5. **Close/flush contract for user-owned buffers.** Libfabric `fi_close` silently discards in-flight ops.
   - Why it matters: ext frames and GPU/dmabuf memory must be reclaimed deterministically.
   - Options: (a) `stop` flushes everything as `CANCELED`, and `destroy` guarantees no later callbacks; (b) `destroy` blocks until all buffers are returned; (c) libfabric semantics (the app waits for `destroy` to return and assumes all buffers are free).
6. **Memory registration object.** Should MTL introduce `mtl_mr_reg(attr{iov, mem_type, device, dmabuf_fd})` and have frames reference `{mr, offset}` instead of raw IOVA (`st20_ext_frame`, `mtl_dma_map`)?
   - Why it matters: it unifies hugepage, user-malloc, CUDA/Level Zero and dmabuf memory, and enables `-EBUSY` refcounted close.
   - Options: (a) yes, mandatory for zero-copy; (b) yes, but keep raw-IOVA frames as a fast path; (c) no, keep `mtl_dma_map`.
7. **Requested vs granted capability negotiation.** Should session create fail when a non-zero requested feature is unavailable (HW pacing, DMA offload, header split, PTP), instead of falling back silently?
   - Why it matters: silent fallback (e.g. DMA → CPU, RL → TSC) hides compliance problems.
   - Options: (a) libfabric rule: non-zero means required, zero means auto, granted values reported; (b) a per-feature `REQUIRED/PREFERRED/OFF` tri-state; (c) keep today's fallback but always report it.
8. **ABI versioning mechanism.** `mtl_init_params` and all `*_ops` are app-allocated, unversioned structs.
   - Why it matters: every new field is an ABI break today.
   - Options: (a) hard-coded API version argument to `mtl_init` (libfabric style); (b) a `size` first member in every public struct; (c) opaque attribute objects with setters (no public struct layouts); (d) linker version script plus compat shims (libfabric `abi_1_0.c`), the heaviest option.
9. **App-driven progress mode.** Should MTL offer a `FI_PROGRESS_MANUAL`-like `mtl_progress(session)` so apps (FFmpeg, GStreamer, OBS) can run MTL without dedicated lcores?
   - Why it matters: it matters for container and low-density use, but is unsafe for hardware-paced HD/UHD video TX.
   - Options: (a) never; (b) RX and low-rate media only; (c) all session types, with documented pacing caveats.
10. **Threading promise.** Which guarantees does the new API give — per-session thread safety (`FI_THREAD_SAFE`) or single-thread-per-session/CQ (`FI_THREAD_COMPLETION`, lock-free)? Current behavior for concurrent `get/put_frame` is unverified.
    - Why it matters: locks on the submit path cost latency, while no locks make multi-threaded apps crash.
    - Options: (a) declare per session at create; (b) always single-threaded per session; (c) always thread-safe.
11. **Where does VSYNC/epoch-tick go?** It is time-critical, yet libfabric's EQ is explicitly not a fast path.
    - Options: (a) data CQ entry type; (b) EQ event; (c) separate low-latency notification (eventfd per session).
12. **Extension mechanism.** Should backend-specific features (DPDK queue meta, `DATA_PATH_ONLY`, AF_XDP knobs, RDMA backend) move to a libfabric-style `mtl_open_ext(handle, "name", version, &ops)`?
    - Why it matters: it keeps the core API backend-neutral and lets experimental features evolve without ABI promises.
    - Options: (a) yes, with named versioned extension tables; (b) flags in the main ops struct, as today; (c) separate experimental headers, as `include/experimental/` does today.
