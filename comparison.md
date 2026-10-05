---
orphan: true
---

# Graphics proposals: select one version for each image

This PR adds proposals for redrawn documentation images. It changes no existing
file: each original PNG or SVG stays, and no doc links to a proposal yet. Use this
file to compare each original with its proposals, then select one in the review.

Open this file in the PR with **View file** (rich diff) to see the images.

## How to select

Write one line for each image in a review comment, for example:

```text
software_stack: v2
tasklet: keep the original
af_xdp: v3
```

A follow-up commit then does these steps for each image:

1. Rename the selected proposal to the final name (for example `software_stack.svg`).
2. Change the link in the doc to the new file.
3. Delete the original and the proposals that are not selected.
4. Delete this file.

## Styles

| Style | Proposals | Rule |
| --- | --- | --- |
| A: `arch.svg` style | `software_stack_v1`, `software_stack_v2`, `rx_dma_offload_v2` | The colors, fonts and boxes of `doc/png/arch.svg`. |
| B: original flow | all other proposals | The layout and flow of the original image, only the five-color palette below, 3 px outlines, 4 to 5 px arrows, text of 18 px or larger. |

The style B palette, plus white:

| Color | Use |
| --- | --- |
| `#1F271B` | Text, lines, and dark fills with white text |
| `#0B4F6C` | Dark fill (teal) with white text |
| `#145C9E` | Dark fill (blue) with white text |
| `#CBB9A8` | Light fill (tan) with dark text |
| `#DCC7BE` | Light fill (sand) with dark text |

If one style is selected for all images, `software_stack` and `rx_dma_offload`
have proposals in both styles.

`doc/png/arch.svg` does not change in this PR.

## Summary

| Image | Used in | Proposals |
| --- | --- | --- |
| `doc/png/software_stack.png` | `doc/design.md:9` | v1, v2 |
| `doc/png/tasklet.png` | `doc/design.md:24` | v5, v6 |
| `doc/png/tx_zero_copy.png` | `doc/design.md:219` | v5 |
| `doc/png/tx_pacing.png` | `doc/design.md:236` | v5 |
| `doc/png/rx_dma_offload.png` | `doc/design.md:255` | v2, v5 |
| `doc/png/mtl-appliance-use-case.png` | `doc/sdm_appliance.md:11` | v5 |
| `doc/png/desktop-streaming-mtl.png` | `doc/sdm_appliance.md:15` | v5, v6 |
| `doc/png/af_xdp.svg` | `doc/xdp.md:22` | v1, v2, v3 |
| `doc/png/xdp-mtl.svg` | `doc/xdp.md:34` | v1, v2, v3 |
| `doc/png/rtcp.svg` | `doc/rtcp.md:18` | v1, v2, v3 |
| `manager/manager_design.svg` | `manager/README.md:5` | v1, v2, v3 |
| `.github/ci_arch.svg` | `.github/github_actions_issue.md:19` | v1, v2, v3 |

The version numbers come from three design rounds, so they are not continuous. A
missing number is a version that was already rejected.

## software_stack

<table>
<tr><th>Original</th><th>v1 (style A)</th><th>v2 (style A)</th></tr>
<tr>
<td><img src="doc/png/software_stack.png" alt="software_stack original" width="300"></td>
<td><img src="doc/png/software_stack_v1.svg" alt="software_stack proposal v1" width="300"></td>
<td><img src="doc/png/software_stack_v2.svg" alt="software_stack proposal v2" width="300"></td>
</tr>
</table>

- **v1**: the diagram of the slide, with the same layout: control plane on the left,
  TX down the middle, RX up the right, layers at the bottom.
- **v2**: v1 plus two cards with the slide text (control plane, data plane).

## tasklet

<table>
<tr><th>Original</th><th>v5</th><th>v6</th></tr>
<tr>
<td><img src="doc/png/tasklet.png" alt="tasklet original" width="300"></td>
<td><img src="doc/png/tasklet_v5.svg" alt="tasklet proposal v5" width="300"></td>
<td><img src="doc/png/tasklet_v6.svg" alt="tasklet proposal v6" width="300"></td>
</tr>
</table>

- **v5**: the slide: scheduler and tasklet boxes and the four benefits on the left,
  the ring of eight tasklets around the scheduler on the right.
- **v6**: the ring only, with run-order numbers 1 to 8, a legend, and a note.

## tx_zero_copy

<table>
<tr><th>Original</th><th>v5</th></tr>
<tr>
<td><img src="doc/png/tx_zero_copy.png" alt="tx_zero_copy original" width="420"></td>
<td><img src="doc/png/tx_zero_copy_v5.svg" alt="tx_zero_copy proposal v5" width="420"></td>
</tr>
</table>

- **v5**: the header mbufs and the external memory buffer of the slide, plus the
  key points on the left.

## tx_pacing

<table>
<tr><th>Original</th><th>v5</th></tr>
<tr>
<td><img src="doc/png/tx_pacing.png" alt="tx_pacing original" width="420"></td>
<td><img src="doc/png/tx_pacing_v5.svg" alt="tx_pacing proposal v5" width="420"></td>
</tr>
</table>

- **v5**: the vertical flow of the slide (Frame, build, TSC / RL / TSN, NIC), plus
  one card for each pacing method on the left.

## rx_dma_offload

<table>
<tr><th>Original</th><th>v2 (style A)</th><th>v5</th></tr>
<tr>
<td><img src="doc/png/rx_dma_offload.png" alt="rx_dma_offload original" width="300"></td>
<td><img src="doc/png/rx_dma_offload_v2.svg" alt="rx_dma_offload proposal v2" width="300"></td>
<td><img src="doc/png/rx_dma_offload_v5.svg" alt="rx_dma_offload proposal v5" width="300"></td>
</tr>
</table>

- **v2**: packets, DMA copy and frame, plus the CPU and DSA speedup chart and the key
  points, in style A.
- **v5**: the slide layout: the bullets and the chart on the left, the copy diagram
  on the right.

## mtl-appliance-use-case

<table>
<tr><th>Original</th><th>v5</th></tr>
<tr>
<td><img src="doc/png/mtl-appliance-use-case.png" alt="mtl-appliance-use-case original" width="420"></td>
<td><img src="doc/png/mtl-appliance-use-case_v5.svg" alt="mtl-appliance-use-case proposal v5" width="420"></td>
</tr>
</table>

- **v5**: the picture of the original (laptop, NUC, switch, SDM PC), plus cards with
  the hardware and the stream settings from `doc/sdm_appliance.md`.

## desktop-streaming-mtl

<table>
<tr><th>Original</th><th>v5</th><th>v6</th></tr>
<tr>
<td><img src="doc/png/desktop-streaming-mtl.png" alt="desktop-streaming-mtl original" width="300"></td>
<td><img src="doc/png/desktop-streaming-mtl_v5.svg" alt="desktop-streaming-mtl proposal v5" width="300"></td>
<td><img src="doc/png/desktop-streaming-mtl_v6.svg" alt="desktop-streaming-mtl proposal v6" width="300"></td>
</tr>
</table>

- **v5**: the picture of the original, plus the same fact cards as
  `mtl-appliance-use-case_v5`.
- **v6**: the picture with numbered steps 1 to 3 and a legend: cable or stream.

## af_xdp

<table>
<tr><th>Original</th><th>v1</th><th>v2</th><th>v3</th></tr>
<tr>
<td><img src="doc/png/af_xdp.svg" alt="af_xdp original" width="160"></td>
<td><img src="doc/png/af_xdp_v1.svg" alt="af_xdp proposal v1" width="220"></td>
<td><img src="doc/png/af_xdp_v2.svg" alt="af_xdp proposal v2" width="260"></td>
<td><img src="doc/png/af_xdp_v3.svg" alt="af_xdp proposal v3" width="240"></td>
</tr>
</table>

- **v1**: the original layout: four rings, two apps, the descriptor and UMEM.
- **v2**: v1 plus cards from `doc/xdp.md`: socket and UMEM, TX side, RX side.
- **v3**: v1 plus the driver half of each loop (dashed), steps 1 to 4 on each side,
  and UMEM drawn as frames.

## xdp-mtl

<table>
<tr><th>Original</th><th>v1</th></tr>
<tr>
<td><img src="doc/png/xdp-mtl.svg" alt="xdp-mtl original" width="420"></td>
<td><img src="doc/png/xdp-mtl_v1.svg" alt="xdp-mtl proposal v1" width="420"></td>
</tr>
<tr><th>v2</th><th>v3</th></tr>
<tr>
<td><img src="doc/png/xdp-mtl_v2.svg" alt="xdp-mtl proposal v2" width="420"></td>
<td><img src="doc/png/xdp-mtl_v3.svg" alt="xdp-mtl proposal v3" width="420"></td>
</tr>
</table>

- **v1**: the original layout, with room for the two maps so that the labels fit.
- **v2**: v1 plus two cards: the AF_XDP datapath and the BPF program filter.
- **v3**: v1 plus steps 1 to 3, the "other packets go to the network stack" path, and
  a legend.

## rtcp

<table>
<tr><th>Original</th><th>v1</th></tr>
<tr>
<td><img src="doc/png/rtcp.svg" alt="rtcp original" width="420"></td>
<td><img src="doc/png/rtcp_v1.svg" alt="rtcp proposal v1" width="420"></td>
</tr>
<tr><th>v2</th><th>v3</th></tr>
<tr>
<td><img src="doc/png/rtcp_v2.svg" alt="rtcp proposal v2" width="420"></td>
<td><img src="doc/png/rtcp_v3.svg" alt="rtcp proposal v3" width="420"></td>
</tr>
</table>

- **v1**: the original sequence diagram. The lost packet has a large X, and the
  retransmission comes out of the packet buffer.
- **v2**: v1 plus two cards: the workflow and the `ops.rtcp` settings from
  `doc/rtcp.md`.
- **v3**: v1 plus steps 1 to 6 in the diagram, a step list and a key.

Each proposal is smaller than 15 KB. The original is 790 KB, because it embeds an
icon image.

## manager_design

<table>
<tr><th>Original</th><th>v1</th></tr>
<tr>
<td><img src="manager/manager_design.svg" alt="manager_design original" width="420"></td>
<td><img src="manager/manager_design_v1.svg" alt="manager_design proposal v1" width="420"></td>
</tr>
<tr><th>v2</th><th>v3</th></tr>
<tr>
<td><img src="manager/manager_design_v2.svg" alt="manager_design proposal v2" width="420"></td>
<td><img src="manager/manager_design_v3.svg" alt="manager_design proposal v3" width="420"></td>
</tr>
</table>

- **v1**: the original layout. All the labels show in full ("Linux Kernel", "AF_XDP
  socket"). The original cuts them off.
- **v2**: v1 plus the key points of `manager/README.md`.
- **v3**: v1 plus steps 1 to 7 on the arrows, a step list and a legend.

## ci_arch

<table>
<tr><th>Original</th><th>v1</th></tr>
<tr>
<td><img src=".github/ci_arch.svg" alt="ci_arch original" width="420"></td>
<td><img src=".github/ci_arch_v1.svg" alt="ci_arch proposal v1" width="420"></td>
</tr>
<tr><th>v2</th><th>v3</th></tr>
<tr>
<td><img src=".github/ci_arch_v2.svg" alt="ci_arch proposal v2" width="420"></td>
<td><img src=".github/ci_arch_v3.svg" alt="ci_arch proposal v3" width="420"></td>
</tr>
</table>

- **v1**: the original flow chart on a light background: PR pipeline on the left,
  artifact stash in the middle, nightly on the right.
- **v2**: v1 plus key points from `doc/cicd_setup_proposition.md`.
- **v3**: v1 plus steps 1 to 13, the main path in blue, one download trunk and a
  legend.

## Items to check before you select

1. **Changed facts.**
   - `tx_pacing_v5`: TSN pacing needs an Intel E830 NIC, a PF port and the
     built-in PTP; E810 has no support. This comes from `doc/design.md` 4.3.3.
     The original said "Available in next generation intel NIC".
   - `ci_arch` (all): "MD5SUM" is now "SHA-256", because `script/hash_sources.sh`
     uses `sha256sum`. The dotted lines from the stash to the hosts say "Download",
     not "UPLOAD".
2. **Code names that differ from the original.**
   - `tasklet_v6` notes that the code has no KNI tasklet. One `cni` tasklet
     (`lib/src/mt_cni.c`) handles ARP and IGMP.
   - The port filter map in the code is `udp4_dp_filter` (`manager/mtl.xdp.c:20`).
     Only `xdp-mtl_v3` uses that name. `xdp-mtl_v1`, `xdp-mtl_v2` and all
     `manager_design` versions keep `udp_filter_map`.
3. **Kept from the original, but check them.**
   - `af_xdp`: the kernel `struct xdp_desc` holds `addr, len, options`, not
     `index, addr, len`. 2048 B is a typical frame size: MTL takes it from the
     mempool.
   - `rtcp`: the dashed "RTCP packet" from TX to RX has no match in
     `lib/src/mt_rtcp.c`. The window values 128 and 10 differ from the doc example
     (`seq_bitmap_size = 64`, `seq_skip_window = 4`).
   - `rx_dma_offload`: the chart values (CPU 1, DSA 2.25) come from the original
     slide. They are not a new measurement.
   - `mtl-appliance-use-case` and `desktop-streaming-mtl`: the pictures use
     192.168.100.32 and .30. The commands in `doc/sdm_appliance.md` use
     `-local_addr 192.168.100.55` on both hosts.
4. **The owner must give the label.** In `ci_arch`, the box "ANDRZEJOS -- JPEGA"
   stays in v1. v2 and v3 write "JPEG-XS (owner check)".
5. **Typos corrected:** asynchronized, Eesy, TR_OFFEST, encapulation,
   de-capulation, Taskslet, ip:/Ip:, NUC11TNki5, windows size, crush, RAPORT.
