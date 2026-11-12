#pragma once
/* NPU / SVP2 person detection (work in progress — see npu.c). */
void npu_init(void);           /* register npu.enabled */
int  npu_start(void);          /* guarded NPU init if npu.enabled; 0 ok */
int  npu_test(void);           /* one guarded init attempt (POST /api/npu/test) */
int  npu_create(int model_type, int classify, int iou, int target);
int  npu_create_async(void);   /* create on a detached thread (the ioctl can hang) */
int  npu_started(void);
