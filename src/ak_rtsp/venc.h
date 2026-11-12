#pragma once

/* Clear stale VENC channel state left by a previously killed anyka_ipc.
 * Safe to call unconditionally — if channel is already clean it's a no-op. */
void venc_reset_stale_channel(void);

/* VENC_IOCTL_QUERY_DMA → CREATE_POOL (ring) → CREATE_POOL (ext) → mmap. */
int venc_create_pool(void);

/* VENC_IOCTL_CREATE_ENC — configure H.264 Main 1920×1080 encoder. */
int venc_create_encoder(void);

/* VENC_IOCTL_BIND_VI_CHN — bind encoder channel 0 to VI channel 0 (kernel mode). */
int venc_bind_vi(void);

/* VENC_IOCTL_ACTIVATE — start frame production. */
int venc_activate(void);

/* Stream settings venc.kbps / venc.gop (take effect at the next start). */
void venc_register_params(void);
void venc_startup_ok(void);        /* first frames streamed: clear the crash guard */
const char *venc_note(void);       /* non-empty if custom settings were skipped */
int  venc_kbps_now(void);          /* values the running encoder was created with */
int  venc_gop_now(void);
int  venc_smart_now(void);         /* smart mode in use: 0 off, 1 ltr, 2 gop */
void venc_request_idr(void);       /* keyframe now (only with smart encoding; max 1/s) */
