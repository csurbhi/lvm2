/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include "metadata.h"
#include "pv_map.h"
#include "log.h"

/*
 * The heart of the allocation code.  This
 * function takes a pv_area and allocates it to
 * the lv.  If the lv doesn't need the complete
 * area then the area is split, otherwise the area
 * is unlinked from the pv_map.
 */
static int _alloc_area(struct logical_volume *lv, uint32_t index,
		       struct physical_volume *pv, struct pv_area *pva)
{
	uint32_t count, remaining, i, start;

	start = pva->start;

	count = pva->count;
	remaining = lv->le_count - index;

	if (remaining < count) {
		/* split the area */
		count = remaining;
		pva->start += count;
		pva->count -= count;

	} else {
		/* unlink the area */
		list_del(&pva->list);
	}

	for (i = 0; i < count; i++) {
		lv->map[i + index].pv = pv;
		lv->map[i + index].pe = start + i;
	}

	return count;
}

static int _alloc_striped(struct logical_volume *lv, struct list *pvms)
{
	log_err("striped allocation not implemented yet.");
	return 0;
}

static int _alloc_contiguous(struct logical_volume *lv, struct list *pvms)
{
	log_err("contiguous allocation not implemented yet.");
	return 0;
}

static int _alloc_simple(struct logical_volume *lv, struct list *pvms)
{
	struct list *tmp1, *tmp2;
	struct pv_map *pvm;
	struct pv_area *pva;
	uint32_t allocated = 0;

	list_iterate (tmp1, pvms) {
		pvm = list_item(tmp1, struct pv_map);

		list_iterate (tmp2, &pvm->areas) {
			pva = list_item(tmp2, struct pv_area);
			allocated += _alloc_area(lv, allocated, pvm->pv, pva);

			if (allocated == lv->le_count)
				break;
		}

		if (allocated == lv->le_count) /* FIXME: yuck, repeated test */
			break;
	}

	if (allocated != lv->le_count) {
		log_err("insufficient free extents to "
			"allocate logical volume");
		return 0;
	}

	return 1;
}

struct logical_volume *lv_create(struct io_space *ios,
				 const char *name,
				 uint32_t status,
				 uint32_t stripes,
				 uint32_t stripe_size,
				 uint32_t extents,
				 struct volume_group *vg,
				 struct pv_list *acceptable_pvs)
{
	struct lv_list *ll = NULL;
	struct logical_volume *lv;
	struct list *pvms;
	struct pool *scratch;
	int r;

	if (!(scratch = pool_create(1024))) {
		stack;
		return NULL;
	}

	if (!extents) {
		log_err("Attempt to create an lv with zero extents");
		return NULL;
	}

	if (vg->free_count < extents) {
		log_err("Insufficient free extents in volume group");
		goto bad;
	}

	if (vg->max_lv == vg->lv_count) {
		log_err("Maximum logical volumes already reached "
			"for this volume group.");
		goto bad;
	}

	if (!(ll = pool_zalloc(ios->mem, sizeof(*ll)))) {
		stack;
		return NULL;
	}

	lv = &ll->lv;

	strcpy(lv->id.uuid, "");

	if (!(lv->name = pool_strdup(ios->mem, name))) {
		stack;
		goto bad;
	}

	lv->vg = vg;
	lv->status = status;
	lv->read_ahead = 0;
	lv->stripes = stripes;
	lv->size = extents * vg->extent_size;
	lv->le_count = extents;

	if (!(lv->map = pool_zalloc(ios->mem, sizeof(*lv->map) * extents))) {
		stack;
		goto bad;
	}

	/*
	 * Build the sets of available areas on
	 * the pv's.
	 */
	if (!(pvms = create_pv_maps(scratch, vg))) {
		log_err("couldn't create extent mappings");
		goto bad;
	}

	if (stripes > 1)
		r = _alloc_striped(lv, pvms);

	else if (status & ALLOC_CONTIGUOUS)
		r = _alloc_contiguous(lv, pvms);

	else
		r = _alloc_simple(lv, pvms);

	if (!r) {
		log_err("Extent allocation failed.");
		goto bad;
	}

	list_add(&ll->list, &vg->lvs);
	vg->lv_count++;
	vg->free_count -= extents;

	pool_destroy(scratch);
	return lv;

 bad:
	if (ll)
		pool_free(ios->mem, ll);

	pool_destroy(scratch);
	return NULL;
}
