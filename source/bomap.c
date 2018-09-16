#include <stdio.h>
#include <stdlib.h>
#include "private.h"

#ifdef DEBUG
#	define TRACE(x...) printf("nouveau: " x)
#	define CALLED() TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#	define TRACE(x...)
# define CALLED()
#endif

static inline unsigned bo_map_hash(struct nouveau_bo *bo)
{
	return bo->handle % BO_MAP_NUM_BUCKETS;
}

static inline struct nouveau_client_bo_map_entry *bo_map_lookup(struct nouveau_client_bo_map *bomap, struct nouveau_bo *bo)
{
	struct nouveau_client_bo_map_entry *ent;
	for (ent = bomap->buckets[bo_map_hash(bo)]; ent; ent = ent->next)
		if (ent->bo_handle == bo->handle)
			break;
	return ent;
}

void
cli_map_free(struct nouveau_client *client)
{
	struct nouveau_client_bo_map *bomap = &nouveau_client(client)->bomap;
	unsigned i;

	// Free all buckets
	for (i = 0; i < BO_MAP_NUM_BUCKETS; i ++) {
		struct nouveau_client_bo_map_entry *ent, *next;
		for (ent = bomap->buckets[i]; ent; ent = next) {
			next = ent->next;
			free(ent);
		}
	}
}

struct drm_nouveau_gem_pushbuf_bo *
cli_kref_get(struct nouveau_client *client, struct nouveau_bo *bo)
{
	struct nouveau_client_bo_map *bomap = &nouveau_client(client)->bomap;
	struct nouveau_client_bo_map_entry *ent = bo_map_lookup(bomap, bo);
	struct drm_nouveau_gem_pushbuf_bo *kref = NULL;
	if (ent)
		kref = ent->kref;
	return kref;
}

struct nouveau_pushbuf *
cli_push_get(struct nouveau_client *client, struct nouveau_bo *bo)
{
	struct nouveau_client_bo_map *bomap = &nouveau_client(client)->bomap;
	struct nouveau_client_bo_map_entry *ent = bo_map_lookup(bomap, bo);
	struct nouveau_pushbuf *push = NULL;
	if (ent)
		push = ent->push;
	return push;
}

void
cli_kref_set(struct nouveau_client *client, struct nouveau_bo *bo,
             struct drm_nouveau_gem_pushbuf_bo *kref,
             struct nouveau_pushbuf *push)
{
	struct nouveau_client_bo_map *bomap = &nouveau_client(client)->bomap;
	struct nouveau_client_bo_map_entry *ent = bo_map_lookup(bomap, bo);

	TRACE("setting 0x%x <-- {%p,%p}\n", bo->handle, kref, push);

	if (!ent) {
		ent = malloc(sizeof(*ent));
		if (!ent) {
			// Shouldn't we panic here?
			TRACE("panic: out of memory\n");
			return;
		}

		// Add entry to bucket list
		unsigned hash = bo_map_hash(bo);
		ent->next = bomap->buckets[hash];
		ent->bo_handle = bo->handle;
		bomap->buckets[hash] = ent;
	}

	ent->kref = kref;
	ent->push = push;
}
