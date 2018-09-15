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
	NvCmdList cmd_list;
	struct nouveau_bo *bos[];
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
pushbuf_dump(struct nouveau_pushbuf_krec *krec, int krec_id, int chid)
{
	struct drm_nouveau_gem_pushbuf_reloc *krel;
	struct drm_nouveau_gem_pushbuf_push *kpsh;
	struct drm_nouveau_gem_pushbuf_bo *kref;
	struct nouveau_bo *bo;
	uint32_t *bgn, *end;
	int i;

	TRACE("ch%d: krec %d pushes %d bufs %d relocs %d\n", chid,
	    krec_id, krec->nr_push, krec->nr_buffer, krec->nr_reloc);

	kref = krec->buffer;
	for (i = 0; i < krec->nr_buffer; i++, kref++) {
		TRACE("ch%d: buf %08x %08x %08x %08x %08x\n", chid, i,
		    kref->handle, kref->valid_domains,
		    kref->read_domains, kref->write_domains);
	}

	krel = krec->reloc;
	for (i = 0; i < krec->nr_reloc; i++, krel++) {
		TRACE("ch%d: rel %08x %08x %08x %08x %08x %08x %08x\n",
		    chid, krel->reloc_bo_index, krel->reloc_bo_offset,
		    krel->bo_index, krel->flags, krel->data,
		    krel->vor, krel->tor);
	}

	kpsh = krec->push;
	for (i = 0; i < krec->nr_push; i++, kpsh++) {
		kref = krec->buffer + kpsh->bo_index;
		bo = (void *)(unsigned long)kref->user_priv;
		bgn = (uint32_t *)((char *)bo->map + kpsh->offset);
		end = bgn + (kpsh->length /4);

		TRACE("ch%d: psh %08x %010llx %010llx\n", chid, kpsh->bo_index,
		    (unsigned long long)kpsh->offset,
		    (unsigned long long)(kpsh->offset + kpsh->length));
		while (bgn < end)
			TRACE("\t0x%08x\n", *bgn++);
	}

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
	struct nouveau_pushbuf_priv *nvpb;
	struct nouveau_pushbuf *push;
	int ret;

	nvpb = calloc(1, sizeof(*nvpb) + nr * sizeof(*nvpb->bos));
	if (!nvpb)
		return -ENOMEM;

	nvpb->krec = calloc(1, sizeof(*nvpb->krec));
	nvpb->list = nvpb->krec;
	if (!nvpb->krec) {
		free(nvpb);
		return -ENOMEM;
	}

	push = &nvpb->base;
	push->client = client;
	push->channel = immediate ? chan : NULL;
	push->flags = NOUVEAU_BO_RD | NOUVEAU_BO_GART | NOUVEAU_BO_MAP;
	nvpb->type = NOUVEAU_BO_GART;

	for (nvpb->bo_nr = 0; nvpb->bo_nr < nr; nvpb->bo_nr++) {
		ret = nouveau_bo_new(client->device, nvpb->type, 0, size,
				     NULL, &nvpb->bos[nvpb->bo_nr]);
		if (ret) {
			nouveau_pushbuf_del(&push);
			return ret;
		}
	}

	DRMINITLISTHEAD(&nvpb->bctx_list);
	*ppush = push;

	return 0;
}

void
nouveau_pushbuf_del(struct nouveau_pushbuf **ppush)
{
	CALLED();
	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(*ppush);
	if (nvpb) {
		struct drm_nouveau_gem_pushbuf_bo *kref;
		struct nouveau_pushbuf_krec *krec;
		while ((krec = nvpb->list)) {
			kref = krec->buffer;
			while (krec->nr_buffer--) {
				unsigned long priv = kref++->user_priv;
				struct nouveau_bo *bo = (void *)priv;
				cli_kref_set(nvpb->base.client, bo, NULL, NULL);
				nouveau_bo_ref(NULL, &bo);
			}
			nvpb->list = krec->next;
			free(krec);
		}
		while (nvpb->bo_nr--)
			nouveau_bo_ref(NULL, &nvpb->bos[nvpb->bo_nr]);
		nouveau_bo_ref(NULL, &nvpb->bo);
		free(nvpb);
	}
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
	struct nouveau_pushbuf_krec *krec = nvpb->krec;
	struct nouveau_client *client = push->client;
	struct nouveau_bo *bo = NULL;
	bool flushed = false;
	int ret = 0;

	/* switch to next buffer if insufficient space in the current one */
	if (push->cur + dwords >= push->end) {
		if (nvpb->bo_next < nvpb->bo_nr) {
			nouveau_bo_ref(nvpb->bos[nvpb->bo_next++], &bo);
			if (nvpb->bo_next == nvpb->bo_nr && push->channel)
				nvpb->bo_next = 0;
		} else {
			ret = nouveau_bo_new(client->device, nvpb->type, 0,
					     nvpb->bos[0]->size, NULL, &bo);
			if (ret)
				return ret;
		}
	}

	/* make sure there's always enough space to queue up the pending
	 * data in the pushbuf proper
	 */
	pushes++;

	/* need to flush if we've run out of space on an immediate pushbuf,
	 * if the new buffer won't fit, or if the kernel push/reloc limits
	 * have been hit
	 */
	if ((bo && ( push->channel ||
		    !pushbuf_kref(push, bo, push->flags))) ||
	    krec->nr_reloc + relocs >= NOUVEAU_GEM_MAX_RELOCS ||
	    krec->nr_push + pushes >= NOUVEAU_GEM_MAX_PUSH) {
		if (nvpb->bo && krec->nr_buffer)
			pushbuf_flush(push);
		flushed = true;
	}

	/* if necessary, switch to new buffer */
	if (bo) {
		ret = nouveau_bo_map(bo, NOUVEAU_BO_WR, push->client);
		if (ret)
			return ret;

		nouveau_pushbuf_data(push, NULL, 0, 0);
		nouveau_bo_ref(bo, &nvpb->bo);
		nouveau_bo_ref(NULL, &bo);

		nvpb->bgn = nvpb->bo->map;
		nvpb->ptr = nvpb->bgn;
		push->cur = nvpb->bgn;
		push->end = push->cur + (nvpb->bo->size / 4);
		push->end -= 2 + push->rsvd_kick; /* space for suffix */
	}

	pushbuf_kref(push, nvpb->bo, push->flags);
	return flushed ? pushbuf_validate(push, false) : 0;
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
