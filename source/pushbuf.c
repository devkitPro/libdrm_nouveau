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
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "libdrm_lists.h"
#include "nouveau_drm.h"
#include "nouveau.h"
#include "private.h"

#include <switch.h>

#ifdef DEBUG
#	define TRACE(x...) printf("nouveau: " x)
#	define CALLED() TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#	define TRACE(x...)
# define CALLED()
#endif

struct nouveau_pushbuf_krec {
	struct nouveau_pushbuf_krec *next;
	struct drm_nouveau_gem_pushbuf_bo buffer[NOUVEAU_GEM_MAX_BUFFERS];
	struct drm_nouveau_gem_pushbuf_reloc reloc[NOUVEAU_GEM_MAX_RELOCS];
	struct drm_nouveau_gem_pushbuf_push push[NOUVEAU_GEM_MAX_PUSH];
	int nr_buffer;
	int nr_reloc;
	int nr_push;
	uint64_t vram_used;
	uint64_t gart_used;
};

struct nouveau_pushbuf_priv {
	struct nouveau_pushbuf base;
	struct nouveau_pushbuf_krec *list;
	struct nouveau_pushbuf_krec *krec;
	struct nouveau_list bctx_list;
	struct nouveau_bo *bo;
	uint32_t type;
	uint32_t *ptr;
	uint32_t *bgn;
	int bo_next;
	int bo_nr;
	//struct nouveau_bo *bos[]; TODO: Array of cmd_list
	NvCmdList cmd_list;
};

static inline struct nouveau_pushbuf_priv *
nouveau_pushbuf(struct nouveau_pushbuf *push)
{
	return (struct nouveau_pushbuf_priv *)push;
}

static int pushbuf_validate(struct nouveau_pushbuf *, bool);
static int pushbuf_flush(struct nouveau_pushbuf *);

static bool
pushbuf_kref_fits(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
		  uint32_t *domains)
{
	CALLED();

	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(push);
	struct nouveau_pushbuf_krec *krec = nvpb->krec;
	struct nouveau_device *dev = push->client->device;

	// Check if the buffer fits in the available GART.
	if (krec->gart_used + bo->size > dev->gart_limit) {
		TRACE("buffer with size %u does not fit in memory. used=%u limit %u\n", 
			bo->size, krec->gart_used, dev->gart_limit);
		return false;
	}
	krec->gart_used += bo->size;
	return true;
}

static struct drm_nouveau_gem_pushbuf_bo *
pushbuf_kref(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
	     uint32_t flags)
{
	CALLED();

	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(push);
	struct nouveau_pushbuf_krec *krec = nvpb->krec;
	struct nouveau_pushbuf *fpush;
	struct drm_nouveau_gem_pushbuf_bo *kref;
	uint32_t domains, domains_wr, domains_rd;

	domains = NOUVEAU_GEM_DOMAIN_GART;

	domains_wr = domains * !!(flags & NOUVEAU_BO_WR);
	domains_rd = domains * !!(flags & NOUVEAU_BO_RD);

	/* if buffer is referenced on another pushbuf that is owned by the
	 * same client, we need to flush the other pushbuf first to ensure
	 * the correct ordering of commands
	 */
	fpush = cli_push_get(push->client, bo);
	if (fpush && fpush != push)
		pushbuf_flush(fpush);

	kref = cli_kref_get(push->client, bo);
	if (kref) {
		/* possible conflict in memory types - flush and retry */
		if (!(kref->valid_domains & domains)) {
			return NULL;
		}

		kref->valid_domains &= domains;
		kref->write_domains |= domains_wr;
		kref->read_domains  |= domains_rd;
	} else {
		if (krec->nr_buffer == NOUVEAU_GEM_MAX_BUFFERS ||
		    !pushbuf_kref_fits(push, bo, &domains))
			return NULL;

		kref = &krec->buffer[krec->nr_buffer++];
		kref->user_priv = (unsigned long)bo;
		kref->handle = bo->handle;
		kref->valid_domains = domains;
		kref->write_domains = domains_wr;
		kref->read_domains = domains_rd;
		kref->presumed.valid = 1;
		kref->presumed.offset = bo->offset;
		kref->presumed.domain = NOUVEAU_GEM_DOMAIN_GART;
		cli_kref_set(push->client, bo, kref, push);
		atomic_inc(&nouveau_bo(bo)->refcnt);
	}

	return kref;
}

#if 0
static uint32_t
pushbuf_krel(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
	     uint32_t data, uint32_t flags, uint32_t vor, uint32_t tor)
{
	CALLED();
	// Unneeded
	return 0;
}
#endif

static void
pushbuf_dump(uint32_t *start, uint32_t *end)
{
#ifdef DEBUG
	for (uint32_t cmd = 0; start < end; start++)
	{
		cmd = *start;
		TRACE("0x%08x\n", cmd);
	}
#endif
}

static int
pushbuf_submit(struct nouveau_pushbuf *push, struct nouveau_object *chan)
{
	CALLED();
	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(push);
	NvGpu *gpu = nvpb->cmd_list.parent;
	NvFence fence;
	Result rc;

	if (push->kick_notify)
		push->kick_notify(push);

	if (nvpb->ptr == push->cur) {
		TRACE("Empty pushbuf submitted\n");
		return 0;
	}

	// Calculate the number of commands to submit
	nvpb->cmd_list.num_cmds = push->cur - nvpb->ptr;
	TRACE("Submitting push buffer %p with %zu commands\n", nvpb->ptr, nvpb->cmd_list.num_cmds);

	rc = nvGpfifoSubmitCmdList(&gpu->gpfifo, &nvpb->cmd_list, 0, &fence);
	if (R_FAILED(rc)) {
		TRACE("nvGpfifo rejected pushbuf: %x\n", rc);
		static bool first_fail = true;
		if (first_fail)
			pushbuf_dump(nvpb->bgn, push->cur);
		first_fail = false;
		return -rc;
	}

	TRACE("Got back fence %d %u\n", (int)fence.id, fence.value);
	nvFenceWait(&fence, -1);
	nvpb->ptr = push->cur;

	// TODO: Implicit fencing
	return 0;
}

static int
pushbuf_flush(struct nouveau_pushbuf *push)
{
	CALLED();
	//struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(push);

	int ret = pushbuf_submit(push, push->channel);

	return ret;
}

#if 0
static void
pushbuf_refn_fail(struct nouveau_pushbuf *push, int sref, int srel)
{
	CALLED();

	// Unimplemented
}
#endif

static int
pushbuf_refn(struct nouveau_pushbuf *push, bool retry,
	     struct nouveau_pushbuf_refn *refs, int nr)
{
	CALLED();

	// Unimplemented
	return 0;
}

static int
pushbuf_validate(struct nouveau_pushbuf *push, bool retry)
{
	CALLED();

	// Unimplemented
	return 0;
}

int
nouveau_pushbuf_new(struct nouveau_client *client, struct nouveau_object *chan,
		    int nr, uint32_t size, bool immediate,
		    struct nouveau_pushbuf **ppush)
{
	CALLED();
	struct nouveau_device_priv *nvdev = nouveau_device(client->device);
	struct nouveau_pushbuf_priv *nvpb;
	struct nouveau_pushbuf *push;
	Result rc;

	nvpb = calloc(1, sizeof(*nvpb)); // + nr * sizeof(*nvpb->bos));
	if (!nvpb)
		return -ENOMEM;

	push = &nvpb->base;
	rc = nvCmdListCreate(&nvpb->cmd_list, &nvdev->gpu, size / 4);
	if (R_FAILED(rc)) {
		TRACE("Failed to create pushbuf NvCmdList!\n");
		free(nvpb);
		return -rc;
	}

	push->channel = chan;
	nvpb->bgn = nvBufferGetCpuAddr(&nvpb->cmd_list.buffer);
	nvpb->ptr = nvpb->bgn;
	push->cur = nvpb->bgn;
	push->end = push->cur + nvpb->cmd_list.max_cmds;
	*ppush = push;

	return 0;
}

void
nouveau_pushbuf_del(struct nouveau_pushbuf **ppush)
{
	CALLED();
	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(*ppush);

	nvCmdListClose(&nvpb->cmd_list);
	free(nvpb);
	*ppush = NULL;
}

struct nouveau_bufctx *
nouveau_pushbuf_bufctx(struct nouveau_pushbuf *push, struct nouveau_bufctx *ctx)
{
	CALLED();

	// Unimplemented
	return NULL;
}

int
nouveau_pushbuf_space(struct nouveau_pushbuf *push,
		      uint32_t dwords, uint32_t relocs, uint32_t pushes)
{
	CALLED();
	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(push);

	if (push->cur + dwords >= push->end) {
		TRACE("Command list is full, need a flush...");
		pushbuf_flush(push);
		nvCmdListReset(&nvpb->cmd_list);
		push->cur = nvpb->bgn;
		nvpb->ptr = nvpb->bgn;
	}

	// Unimplemented
	return 0;
}

void
nouveau_pushbuf_data(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
		     uint64_t offset, uint64_t length)
{
	CALLED();

	// Unimplemented
}

int
nouveau_pushbuf_refn(struct nouveau_pushbuf *push,
		     struct nouveau_pushbuf_refn *refs, int nr)
{
	CALLED();
	return pushbuf_refn(push, true, refs, nr);
}

void
nouveau_pushbuf_reloc(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
		      uint32_t data, uint32_t flags, uint32_t vor, uint32_t tor)
{
	CALLED();

	// Unimplemented
}

int
nouveau_pushbuf_validate(struct nouveau_pushbuf *push)
{
	CALLED();
	return pushbuf_validate(push, true);
}

uint32_t
nouveau_pushbuf_refd(struct nouveau_pushbuf *push, struct nouveau_bo *bo)
{
	CALLED();

	// Unimplemented
	return 0;
}

int
nouveau_pushbuf_kick(struct nouveau_pushbuf *push, struct nouveau_object *chan)
{
	CALLED();
	if (!push->channel)
		return pushbuf_submit(push, chan);
	pushbuf_flush(push);
	return pushbuf_validate(push, false);
}
