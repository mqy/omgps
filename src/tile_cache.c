#include <pthread.h>

#include "omgps.h"

/**
 * Cache tile image. The image is loaded into memory as a GDK object (pixbuf or image).
 *
 * According the nature of map navigating, this cache slots are organized into
 * a LRU linked list. The capacity is a fixed value.
 * (1) To find a slot, traverse the list from head to tail.
 * (2) To add a slot, find if it exists, if not add to tail, if no space, remove the head.
 * A normal jpg (256 * 256) map tile takes about (4~15 KB)
 *
 * Call new_tilecache when start or map repo is changed at runtime.
 */

typedef struct __tilecache_slot_t
{
	tile_t *tile;
	struct __tilecache_slot_t *next;
} tilecache_slot_t;

static void free_slot_tile(tile_t *tile)
{
	assert(tile);

	if (tile->pixbuf)
		g_object_unref(tile->pixbuf);
	tile->pixbuf = NULL;
	tile->cached = FALSE;
	free(tile);
}

tilecache_t * tilecache_new(int capacity)
{
	tilecache_t *cache = (tilecache_t*)malloc(sizeof(tilecache_t));
	if (cache) {
		cache->count = 0;
		cache->capacity = capacity;
		cache->head = cache->tail = NULL;
		pthread_mutex_init(&(cache->lock), NULL);
	}

	return cache;
}

void tilecache_cleanup(tilecache_t *cache, gboolean free_cache)
{
	if (! cache)
		return;

	LOCK_MUTEX(&cache->lock);

	tilecache_slot_t *slot = cache->head, *next;
	while(slot) {
		next = slot->next;
		free_slot_tile(slot->tile);
		free(slot);
		slot = next;
	}
	cache->head = cache->tail = NULL;
	cache->count = 0;

	UNLOCK_MUTEX(&cache->lock);

	if (free_cache) {
		pthread_mutex_destroy(&(cache->lock));
		free(cache);
	}
}

tile_t* tilecache_get(tilecache_t *cache, int zoom, int x, int y)
{
	LOCK_MUTEX(&cache->lock);

	tilecache_slot_t *slot = cache->head;
	tile_t *tile = NULL;
	gboolean found = FALSE;

	for (; slot; slot=slot->next) {
		tile = slot->tile;
		if (tile->zoom == zoom && tile->x == x && tile->y == y) {
			found = TRUE;
			//log_debug("tilecache_get: x=%d, y=%d, zoom=%d, found", x, y, zoom);
			break;
		}
	}

	UNLOCK_MUTEX(&cache->lock);

	if (found)
		return tile;
	else
		return NULL;
}

gboolean tilecache_add(tilecache_t *cache, tile_t *tile)
{
	assert(tile && tile->pixbuf);

	LOCK_MUTEX(&cache->lock);

	tilecache_slot_t *slot = NULL;

	if (cache->count >= cache->capacity) {
		/* purge head */
		slot = cache->head;
		cache->head = slot->next;
		free_slot_tile(slot->tile);
		--cache->count;
		//log_debug("tilecache_add: x=%d, y=%d, zoom=%d, head purged", tile->x, tile->y, tile->zoom);
	} else {
		/* create new slot */
		slot = (tilecache_slot_t *)malloc(sizeof(tilecache_slot_t));
		//log_debug("tilecache_add: x=%d, y=%d, zoom=%d, new slot", tile->x, tile->y, tile->zoom);
	}

	if (slot) {
		tile->cached = TRUE;
		slot->tile = tile;
		slot->next = NULL;

		/* enqueue to tail */
		if(cache->tail)
			cache->tail->next = slot;
		else
			cache->head = cache->tail = slot;

		cache->tail = slot;

		++cache->count;
	}

	UNLOCK_MUTEX(&cache->lock);

	return (slot != NULL);
}
