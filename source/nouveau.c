/*
 * Copyright 2012 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>

#include "libdrm_lists.h"
#include "libdrm_atomics.h"
#include "nouveau_drm.h"
#include "nouveau.h"
#include "private.h"

#include "nvif/class.h"
#include "nvif/cl0080.h"
#include "nvif/ioctl.h"
#include "nvif/unpack.h"

#ifdef DEBUG
#	define TRACE(x...) printf("nouveau: " x)
#	define CALLED() TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#	define TRACE(x...)
# define CALLED()
#endif

/* Unused
int
nouveau_object_mthd(struct nouveau_object *obj,
		    uint32_t mthd, void *data, uint32_t size)
{
	return 0;
}
*/

/* Unused
void
nouveau_object_sclass_put(struct nouveau_sclass **psclass)
{
}
*/

/* Unused
int
nouveau_object_sclass_get(struct nouveau_object *obj,
			  struct nouveau_sclass **psclass)
{
	return 0;
}
*/

int
nouveau_object_mclass(struct nouveau_object *obj,
		      const struct nouveau_mclass *mclass)
{
  // TODO: Only used for VP3 firmware upload
	CALLED();
	return 0;
}

/* NVGPU_IOCTL_CHANNEL_ALLOC_OBJ_CTX */
int
nouveau_object_new(struct nouveau_object *parent, uint64_t handle,
		   uint32_t oclass, void *data, uint32_t length,
		   struct nouveau_object **pobj)
{
	struct nouveau_object *obj;
	CALLED();

	if (!(obj = calloc(1, sizeof(*obj))))
		return -ENOMEM;

	if (oclass == NOUVEAU_FIFO_CHANNEL_CLASS)
	{
		struct nouveau_fifo *fifo;
		if (!(fifo = calloc(1, sizeof(*fifo)))) {
			free(obj);
			return -ENOMEM;
		}
		fifo->object = parent;
		fifo->channel = 0;
		fifo->pushbuf = 0;
		obj->data = fifo;
		obj->length = sizeof(*fifo);
	}

	obj->parent = parent;
	obj->oclass = oclass;
	*pobj = obj;
	return 0;
}

/* NVGPU_IOCTL_CHANNEL_FREE_OBJ_CTX */
void
nouveau_object_del(struct nouveau_object **pobj)
{
	CALLED();
	if (!pobj)
		return;

	struct nouveau_object *obj = *pobj;
	if (!obj)
		return;

	if (obj->data)
		free(obj->data);
	free(obj);
	*pobj = NULL;
}

void
nouveau_drm_del(struct nouveau_drm **pdrm)
{
	CALLED();
	struct nouveau_drm *drm = *pdrm;
	free(drm);
	*pdrm = NULL;
}

int
nouveau_drm_new(int fd, struct nouveau_drm **pdrm)
{
	CALLED();
	struct nouveau_drm *drm;
	if (!(drm = calloc(1, sizeof(*drm)))) {
		return -ENOMEM;
	}

	drm->fd = fd;
	drm->version = 0x01000202;
	*pdrm = drm;
	return 0;
}

int
nouveau_device_new(struct nouveau_object *parent, int32_t oclass,
		   void *data, uint32_t size, struct nouveau_device **pdev)
{
	struct nouveau_drm *drm = nouveau_drm(parent);
	struct nouveau_device_priv *nvdev;
	Result rc;
	CALLED();

	if (!(nvdev = calloc(1, sizeof(*nvdev))))
		return -ENOMEM;
	*pdev = &nvdev->base;
	nvdev->base.object.parent = &drm->client;
	nvdev->base.object.handle = ~0ULL;
	nvdev->base.object.oclass = NOUVEAU_DEVICE_CLASS;
	nvdev->base.object.length = ~0;
	nvdev->base.chipset = 0x120; // NVGPU_GPU_ARCH_GM200

	rc = nvGpuCreate(&nvdev->gpu);
	if (R_FAILED(rc))
	{
		TRACE("Failed to create GPU.");
		return -rc;
	}

	mutexInit(&nvdev->lock);
	return 0;
}

void
nouveau_device_del(struct nouveau_device **pdev)
{
	CALLED();
	struct nouveau_device_priv *nvdev = nouveau_device(*pdev);

	nvGpuClose(&nvdev->gpu);

	if (nvdev) {
		free(nvdev->client);
		free(nvdev);
		*pdev = NULL;
	}
}

int
nouveau_getparam(struct nouveau_device *dev, uint64_t param, uint64_t *value)
{
	/* NOUVEAU_GETPARAM_PTIMER_TIME = NVGPU_GPU_IOCTL_GET_GPU_TIME */
	int ret = 0;
	if (param == NOUVEAU_GETPARAM_GRAPH_UNITS)
		*value = (16 << 8) | 4;
	else if (param == NOUVEAU_GETPARAM_PCI_DEVICE)
		*value = 0; // dummy
	else
		ret = -EINVAL;
	return ret;
}

/* Unused
int
nouveau_setparam(struct nouveau_device *dev, uint64_t param, uint64_t value)
{
	return 0;
}
*/

int
nouveau_client_new(struct nouveau_device *dev, struct nouveau_client **pclient)
{
	struct nouveau_device_priv *nvdev = nouveau_device(dev);
	struct nouveau_client_priv *pcli;
	int id = 0, i, ret = -ENOMEM;
	uint32_t *clients;
	CALLED();

	mutexLock(&nvdev->lock);

	for (i = 0; i < nvdev->nr_client; i++) {
		id = ffs(nvdev->client[i]) - 1;
		if (id >= 0)
			goto out;
	}

	clients = realloc(nvdev->client, sizeof(uint32_t) * (i + 1));
	if (!clients)
		goto unlock;
	nvdev->client = clients;
	nvdev->client[i] = 0;
	nvdev->nr_client++;

out:
	pcli = calloc(1, sizeof(*pcli));
	if (pcli) {
		nvdev->client[i] |= (1 << id);
		pcli->base.device = dev;
		pcli->base.id = (i * 32) + id;
		ret = 0;
	}

	*pclient = &pcli->base;

unlock:
	mutexUnlock(&nvdev->lock);
	return ret;
}

void
nouveau_client_del(struct nouveau_client **pclient)
{
	struct nouveau_client_priv *pcli = nouveau_client(*pclient);
	struct nouveau_device_priv *nvdev;
	CALLED();
	if (pcli) {
		int id = pcli->base.id;
		nvdev = nouveau_device(pcli->base.device);
		mutexLock(&nvdev->lock);
		nvdev->client[id / 32] &= ~(1 << (id % 32));
		mutexUnlock(&nvdev->lock);
		cli_map_free(&pcli->base);
		free(pcli);
	}
}

static int
nouveau_bo_fence_wait(struct nouveau_bo *bo, uint32_t access)
{
	CALLED();
	struct nouveau_bo_priv *nvbo = nouveau_bo(bo);
	int ret = 0;

	if ((s32)nvbo->fence.id >= 0) {
		TRACE("waiting on fence {%d,%u}\n", (int)nvbo->fence.id, nvbo->fence.value);
		Result res = nvFenceWait(&nvbo->fence, (access & NOUVEAU_BO_NOBLOCK) ? 0 : -1);
		if (R_FAILED(res))
			ret = -EAGAIN;
		else {
			// Reset the fence since we're done with it.
			nvbo->fence.id = -1;
			nvbo->fence.value = 0;

			// TODO: Check for NOUVEAU_BO_WR - maybe we're supposed to flush cache?
		}
	}

	if (ret == 0)
		nvbo->access = 0;
	return ret;
}

static void
nouveau_bo_del(struct nouveau_bo *bo)
{
	CALLED();
	struct nouveau_bo_priv *nvbo = nouveau_bo(bo);

	nouveau_bo_fence_wait(bo, 0);
	if (nvbo->buffer.has_init)
		nvBufferFree(&nvbo->buffer);
	free(nvbo);
}

int
nouveau_bo_new(struct nouveau_device *dev, uint32_t flags, uint32_t align,
	       uint64_t size, union nouveau_bo_config *config,
	       struct nouveau_bo **pbo)
{
	CALLED();
	struct nouveau_device_priv *nvdev = nouveau_device(dev);

	struct nouveau_bo_priv *nvbo = calloc(1, sizeof(*nvbo));
	struct nouveau_bo *bo = &nvbo->base;
	Result rc;

	if (align == 0)
		align = 0x1000;

	if (!nvbo)
		return -ENOMEM;

	NvKind kind = NvKind_Pitch;
	if (config)
		kind = (NvKind)config->nvc0.memtype;

	TRACE("Allocating BO of size %ld, align %d, flags 0x%x and kind 0x%x\n", size, align, flags, kind);
	rc = nvBufferCreate(&nvbo->buffer, size, align, false, !(flags & NOUVEAU_BO_COHERENT), kind, &nvdev->gpu.addr_space);
	if (R_FAILED(rc))
	{
		TRACE("Failed to create NvBuffer (%x)\n", rc);
		free(nvbo);
		return -rc;
	}

	if (kind != NvKind_Pitch)
	{
		rc = nvBufferMapAsTexture(&nvbo->buffer, kind);
		if (R_FAILED(rc))
		{
			TRACE("Failed to map NvBuffer as texture (%x)\n", rc);
			free(nvbo);
			return -rc;
		}
	}

	atomic_set(&nvbo->refcnt, 1);
	bo->device = dev;
	bo->handle = nvbo->buffer.fd;
	bo->size = nvbo->buffer.size;
	bo->flags = flags;
	bo->offset = kind != NvKind_Pitch ? nvBufferGetGpuAddrTexture(&nvbo->buffer) : nvBufferGetGpuAddr(&nvbo->buffer);
	bo->map = NULL;
	nvbo->map_addr = nvBufferGetCpuAddr(&nvbo->buffer);
	nvbo->fence.id = UINT32_MAX;
	memset(nvbo->map_addr, 0, bo->size);

	if (config)
		bo->config = *config;
	*pbo = bo;
	return 0;
}

/* Unused
static int
nouveau_bo_wrap_locked(struct nouveau_device *dev, uint32_t handle,
		       struct nouveau_bo **pbo, int name)
{
	return 0;
}

static void
nouveau_bo_make_global(struct nouveau_bo_priv *nvbo)
{
}
*/

int
nouveau_bo_wrap(struct nouveau_device *dev, uint32_t handle,
		struct nouveau_bo **pbo)
{
	// TODO: NV30-only
	CALLED();
	return 0;
}

int
nouveau_bo_name_ref(struct nouveau_device *dev, uint32_t name,
		    struct nouveau_bo **pbo)
{
	CALLED();
	struct nouveau_device_priv *nvdev = nouveau_device(dev);
	struct nouveau_bo_priv *nvbo = calloc(1, sizeof(*nvbo));
	struct nouveau_bo *bo = &nvbo->base;
	Result rc;

	NvKind kind = NvKind_Generic_16BX2; // NvKind_C32_2C or NvKind_C32_2CRA could be used here, but they need special support that nouveau seems to lack.
	rc = nvAddressSpaceMap(&nvdev->gpu.addr_space, name, true, kind, &bo->offset);
	if (R_FAILED(rc))
	{
		TRACE("Failed to map named buffer (%x)\n", rc);
		free(nvbo);
		return -rc;
	}

	atomic_set(&nvbo->refcnt, 1);
	bo->device = dev;
	bo->handle = name;
	nvbo->fence.id = UINT32_MAX;
	*pbo = bo;

	bo->config.nvc0.memtype = kind;
	bo->config.nvc0.tile_mode = 0x040;
	return 0;
}

int
nouveau_bo_name_get(struct nouveau_bo *bo, uint32_t *name)
{
	// TODO: Unimplemented
	CALLED();
	return 0;
}

void
nouveau_bo_ref(struct nouveau_bo *bo, struct nouveau_bo **pref)
{
	CALLED();
	struct nouveau_bo *ref = *pref;
	if (bo) {
		atomic_inc(&nouveau_bo(bo)->refcnt);
	}
	if (ref) {
		if (atomic_dec_and_test(&nouveau_bo(ref)->refcnt))
			nouveau_bo_del(ref);
	}
	*pref = bo;
}

int
nouveau_bo_prime_handle_ref(struct nouveau_device *dev, int prime_fd,
			    struct nouveau_bo **bo)
{
	// TODO: Unimplemented
	CALLED();
	return 0;
}

int
nouveau_bo_set_prime(struct nouveau_bo *bo, int *prime_fd)
{
	// TODO: Unimplemented
	CALLED();
	return 0;
}

int
nouveau_bo_get_syncpoint(struct nouveau_bo *bo, unsigned int *out_threshold)
{
	CALLED();
	struct nouveau_bo_priv *nvbo = nouveau_bo(bo);

	if (out_threshold)
		*out_threshold = nvbo->fence.value;

	return nvbo->fence.id;
}

int
nouveau_bo_wait(struct nouveau_bo *bo, uint32_t access,
		struct nouveau_client *client)
{
	CALLED();
	struct nouveau_bo_priv *nvbo = nouveau_bo(bo);
	struct nouveau_pushbuf *push;

	if (!(access & NOUVEAU_BO_RDWR))
		return 0;

	push = cli_push_get(client, bo);
	if (push && push->channel)
		nouveau_pushbuf_kick(push, push->channel);

	if (!(nvbo->access & NOUVEAU_BO_WR) && !(access & NOUVEAU_BO_WR))
		return 0;

	return nouveau_bo_fence_wait(bo, access);
}

int
nouveau_bo_map(struct nouveau_bo *bo, uint32_t access,
	       struct nouveau_client *client)
{
	CALLED();
	struct nouveau_bo_priv *nvbo = nouveau_bo(bo);
	bo->map = nvbo->map_addr;
	return nouveau_bo_wait(bo, access, client);
}

void
nouveau_bo_unmap(struct nouveau_bo *bo)
{
	CALLED();
	bo->map = NULL;
}
