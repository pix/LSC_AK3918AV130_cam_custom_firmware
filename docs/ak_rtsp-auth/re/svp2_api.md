# Anyka SVP2 / NPU person & face detection API (from the public SDK)

Source: SDK tree `KD0000001/ak3918av100` on GitHub — `platform/include/ak_svp2.h` and
`platform/sample/svp2/ak_svp2_sample.c` (AnyCloud39AV100 SDK, same SVP2 family as our AV130).
Cross-checked against our stock `anyka_ipc` wrappers and `ak_npu.ko` (see npu.c notes).

## Calls and ioctls

| SDK call | ioctl (userspace wrapper) | size | notes |
|---|---|---|---|
| `ak_npu_init()` | `0xc0044e11` | 4 | open `/dev/npu`, `_IOWR`, value 0 |
| `ak_svp2_create_chn(chn, attr, conf)` | `0xc09c4e20` | 156 | struct below |
| `ak_svp2_process(chn, input, output)` | `0xc03c4e22` | 60 | in/out |
| `ak_svp2_release(output)` | `0xc00c4e24` | 12 | frees the result buffer |
| `ak_npu_deinit()` | — | | close |

Create request (156 bytes, as built by anyka_ipc @0x1dbb2c; the driver logs the path at +8
and the env mode at +136, which fixes the layout):

```c
struct svp2_create_req {
    int  handle_id;        /* +0  */
    int  model_num;        /* +4  */
    char model_path[128];  /* +8  */   /* /usr/local/model_0x0A000000_V1.1.0.bin */
    int  env_mode;         /* +136 0 day, 1 night */
    int  model_type;       /* +140 0 HD_180P, 1 HD_360P, 2 FACE_DT_180P, ... */
    int  classify_threshold; /* +144 0..1000 */
    int  IoU_threshold;    /* +148 0..10 */
    int  target_type;      /* +152 0 shape, 1 face, 2 shape+face, 3 pet, ... */
};                         /* 156 total, one model */
```

Stock anyka_ipc calls it with `{1, 0, mode, (500 - sensitivity) * 2, 3, 2}` as the 24-byte
attribute (model_num 1, env 0, mode = 0/1/10, target 2 = shape+face) and the model path.

## Frame input

`AK_SVP2_IMG_INFO_T` (inferred field offsets match anyka_ipc's validation @0x1dc8c0:
img_type<=1, width/height and left+width < 16384, vir_addr non-null):

```c
struct svp2_img_info {
    int model_index;   /* +0  */
    int img_type;      /* +4  0 = YUV420SP, 1 = RGB line-interleaved */
    int width;         /* +8  */
    int height;        /* +12 */
    int left;          /* +16 */
    int top;           /* +20 */
    int width_roi;     /* +24 */
    int height_roi;    /* +28 */
    unsigned long phy_addr; /* +32 */
    void *vir_addr;    /* +36 */
};
```

The SDK sample feeds VI channel 16: 320x180 (HD_180P), 480x270 or 640x360 (HD_360P) frames,
NV12 (`YUV420SP`), and runs the detector on every 3rd frame (`ak_vi_get_frame` →
`ak_svp2_process` → `ak_svp2_release`).

## Results

```c
struct svp2_rect { int label; unsigned long left, top, right, bottom, score; }; /* 24 bytes */
struct svp2_hd_result { int total_num; struct svp2_rect *target_boxes; };
struct svp2_output { union { hd_result, fas_result, fr_result, gesture_result } result;
                     int model_type; int model_index; };
```

`label` = target type (shape / face), coordinates in the model's input resolution (e.g. 180p
or 360p), scale to the stream size. `ak_svp2_release` must be called for every successful
`process`.

## Model

The stock APP ships `/usr/local/model_0x0A000000_V1.1.0.bin` (model id 0x0A000000 = HD
person/face at 180p). Our firmware keeps that file (only `bin/ak_rtsp` is replaced). The
driver validates the model id against the request's model type; a mismatched file is refused
with "Load face detect model file failed" (seen in SDK troubleshooting logs).

## Plan for ak_rtsp

1. npu.c: create request as above, feed VI channel 16 frames (either reuse our existing TD
   channel or add one at 320x180 NV12), process every 3rd frame, parse results, release.
2. Crash guard around create/process (already in place for init).
3. Publish events as `person` (count, boxes, score) to MQTT/HA, webhook, ONVIF and the web UI.

## Process request (60 bytes), decoded from anyka_ipc @0x1dc8c0

```
+0   u32  channel info field ([info+44], set at create time)
+4   u32  model_index        (from IMG_INFO +0)
+8   u32  img_type           (+4)
+12  u32  width              (+8)
+16  u32  height             (+12)
+20  u32  left               (+16)
+24  u32  top                (+20)
+28  u32  roi width          (+24)
+32  u32  roi height         (+28)
+36  u32  phy_addr           (+32)
+40  u32  0
+44  u32  0
+48  u32  4                  (constant in the stock wrapper)
+52  u32  [info+48]          (result buffer address for this channel)
+56  ptr  &{u32 count; ptr boxes}   (kernel writes the 16-byte result here)
```

After the ioctl the wrapper reads `model_type` and `model_index` at `[info+48]+4096+708`
and `+712`, and for HD model types (3, 4, 16, 23) copies `{count, boxes}` from
`[info+48]` (24-byte boxes, see above). `ak_svp2_release` (12 bytes) frees that result.

Still unknown: the create-time exchange that yields `[info+44]` and `[info+48]` (the
channel's result buffer). The create wrapper logs each step and allocates a per-channel
info block (stored at ctx + chn*4 + 12, 52-byte alloc + count*20+4 config copy). Mapping
that (which field of the 156-byte reply or which follow-up ioctl provides the buffer) is
the last piece before a first end-to-end frame can be run.

## Hardware finding 2026-11-12: create ioctl blocks

Calling `0xc09c4e20` with the request above (handle 0, one model, path, HD_360P, thresholds 700/3/2)
did not return: the calling thread stayed inside the driver (web requests on other threads
still worked, uptime continuous). Killing the process and letting the supervisor restart it
recovered the camera; the guard file kept the NPU off afterwards. The driver therefore
accepts a request it cannot complete with these parameters — likely the model/parameter
combination (target_type 2 needs shape+face but one model) or a missing pre-step (clock/power
or a model-upload command among 0xc0044e10 / 0xc0084e21 / 0x00004e12), rather than a wrong
size/layout. Next attempt: run create on a detached thread only (npu_create_async does this),
try target_type 0 (shape only) and model_type 0 (HD_180P), and check `dmesg` with the
driver's debug level raised before concluding anything about the layout.
