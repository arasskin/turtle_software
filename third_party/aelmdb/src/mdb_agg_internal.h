/* Internal aggregate representation and arithmetic.
 *
 * This file is textually included by mdb.c after MDB_node and page-layout
 * internals are defined.  It is not a public header and deliberately relies
 * on LMDB private types/macros.
 */

/* Aggregate branch-node prefix layout.  Component bytes are packed in schema
 * order, then the entire prefix is padded to EVEN() so the following key starts
 * at the same 2-byte alignment used throughout LMDB.  Accesses to u64 fields
 * use memcpy and therefore do not assume stronger alignment of MDB_node. */
#define MDB_AGG_U64_SIZE ((size_t)sizeof(uint64_t))
/* Semantic aggregate bytes and physical prefix bytes are deliberately distinct:
 * the semantic blob is packed, while the branch-node prefix is EVEN-padded so
 * the following key keeps LMDB's 2-byte alignment invariant. */
#define MDB_AGG_MAXSZ (MDB_AGG_U64_SIZE + MDB_AGG_U64_SIZE + MDB_HASH_SIZE)
typedef struct MDB_aggblob {
	uint8_t v[MDB_AGG_MAXSZ];
} MDB_aggblob;

/* Normalized semantic aggregate.  Unlike MDB_aggblob this representation is
 * independent of the selected packed fields.  Disabled components are kept at
 * zero by decode/construction helpers and are ignored by arithmetic. */
typedef struct MDB_aggval {
	uint64_t entries;
	uint64_t keys;
	uint8_t hashsum[MDB_HASH_SIZE];
} MDB_aggval;

/* Net logical mutation.  Structural page rewrites do not create an additional
 * change: every ancestor observes A' = A - before + after. */
typedef struct MDB_aggchange {
	MDB_aggval before;
	MDB_aggval after;
} MDB_aggchange;

static inline void
mdb_aggval_zero(MDB_aggval *val)
{
	memset(val, 0, sizeof(*val));
}

static inline int
mdb_aggval_equal(uint16_t agg, const MDB_aggval *a, const MDB_aggval *b)
{
	agg &= MDB_AGG_MASK;
	if ((agg & MDB_AGG_ENTRIES) && a->entries != b->entries)
		return 0;
	if ((agg & MDB_AGG_KEYS) && a->keys != b->keys)
		return 0;
	if ((agg & MDB_AGG_HASHSUM) &&
		memcmp(a->hashsum, b->hashsum, MDB_HASH_SIZE) != 0)
		return 0;
	return 1;
}

/* MDB_aggblob stores only enabled semantic fields, in persistent schema order.
 * Physical EVEN() padding is deliberately not part of this conversion. */
static inline void
mdb_aggval_from_blob(uint16_t agg, const MDB_aggblob *blob, MDB_aggval *val)
{
	const uint8_t *p = blob->v;

	agg &= MDB_AGG_MASK;
	mdb_aggval_zero(val);
	if (agg & MDB_AGG_ENTRIES) {
		memcpy(&val->entries, p, MDB_AGG_U64_SIZE);
		p += MDB_AGG_U64_SIZE;
	}
	if (agg & MDB_AGG_KEYS) {
		memcpy(&val->keys, p, MDB_AGG_U64_SIZE);
		p += MDB_AGG_U64_SIZE;
	}
	if (agg & MDB_AGG_HASHSUM)
		memcpy(val->hashsum, p, MDB_HASH_SIZE);
}

static inline void
mdb_aggval_to_blob(uint16_t agg, const MDB_aggval *val, MDB_aggblob *blob)
{
	uint8_t *p = blob->v;

	agg &= MDB_AGG_MASK;
	memset(blob, 0, sizeof(*blob));
	if (agg & MDB_AGG_ENTRIES) {
		memcpy(p, &val->entries, MDB_AGG_U64_SIZE);
		p += MDB_AGG_U64_SIZE;
	}
	if (agg & MDB_AGG_KEYS) {
		memcpy(p, &val->keys, MDB_AGG_U64_SIZE);
		p += MDB_AGG_U64_SIZE;
	}
	if (agg & MDB_AGG_HASHSUM)
		memcpy(p, val->hashsum, MDB_HASH_SIZE);
}

/* DrawDo format: hash aggregation is bytewise XOR, its own inverse. */
static inline void
mdb_agg_hash_add(uint8_t dst[MDB_HASH_SIZE], const uint8_t src[MDB_HASH_SIZE])
{
    size_t i;
    for (i = 0; i < MDB_HASH_SIZE; ++i) dst[i] ^= src[i];
}
static inline void
mdb_agg_hash_sub(uint8_t dst[MDB_HASH_SIZE], const uint8_t src[MDB_HASH_SIZE])
{
    mdb_agg_hash_add(dst, src);
}

/* Count overflow/underflow cannot occur for a valid maintainable tree.  Treat
 * it as an aggregate invariant failure and leave the destination unchanged. */
static inline int
mdb_aggval_add(uint16_t agg, MDB_aggval *dst, const MDB_aggval *src)
{
	MDB_aggval next = *dst;

	agg &= MDB_AGG_MASK;
	if ((agg & MDB_AGG_ENTRIES) && UINT64_MAX - next.entries < src->entries)
		return MDB_CORRUPTED;
	if ((agg & MDB_AGG_KEYS) && UINT64_MAX - next.keys < src->keys)
		return MDB_CORRUPTED;
	if (agg & MDB_AGG_ENTRIES)
		next.entries += src->entries;
	if (agg & MDB_AGG_KEYS)
		next.keys += src->keys;
	if (agg & MDB_AGG_HASHSUM)
		mdb_agg_hash_add(next.hashsum, src->hashsum);
	*dst = next;
	return MDB_SUCCESS;
}

static inline int
mdb_aggval_sub(uint16_t agg, MDB_aggval *dst, const MDB_aggval *src)
{
	MDB_aggval next = *dst;

	agg &= MDB_AGG_MASK;
	if ((agg & MDB_AGG_ENTRIES) && next.entries < src->entries)
		return MDB_CORRUPTED;
	if ((agg & MDB_AGG_KEYS) && next.keys < src->keys)
		return MDB_CORRUPTED;
	if (agg & MDB_AGG_ENTRIES)
		next.entries -= src->entries;
	if (agg & MDB_AGG_KEYS)
		next.keys -= src->keys;
	if (agg & MDB_AGG_HASHSUM)
		mdb_agg_hash_sub(next.hashsum, src->hashsum);
	*dst = next;
	return MDB_SUCCESS;
}

static inline int
mdb_aggval_apply_change(uint16_t agg, MDB_aggval *dst,
	const MDB_aggchange *change)
{
	MDB_aggval next = *dst;
	int rc;

	rc = mdb_aggval_sub(agg, &next, &change->before);
	if (rc)
		return rc;
	rc = mdb_aggval_add(agg, &next, &change->after);
	if (rc)
		return rc;
	*dst = next;
	return MDB_SUCCESS;
}

/* Extract the configured fixed-width hash source slice.  Negative offsets are
 * relative to the last complete MDB_HASH_SIZE-byte slice: -1 selects the last
 * slice, -2 starts one byte earlier, and so on. */
static inline int
mdb_agg_hash_slice(const MDB_val *source, int hash_offset,
	uint8_t out[MDB_HASH_SIZE])
{
	size_t start, back;

	if (!source || (source->mv_size && !source->mv_data))
		return EINVAL;
	if (hash_offset >= 0) {
		start = (size_t)hash_offset;
		if (start > source->mv_size ||
			MDB_HASH_SIZE > source->mv_size - start)
			return MDB_BAD_VALSIZE;
	} else {
		/* -(hash_offset + 1) is safe for the configured int16_t range and
		 * maps -1 -> 0, -2 -> 1, ... without signed overflow. */
		back = (size_t)(-(hash_offset + 1));
		if (source->mv_size < MDB_HASH_SIZE ||
			back > source->mv_size - MDB_HASH_SIZE)
			return MDB_BAD_VALSIZE;
		start = source->mv_size - MDB_HASH_SIZE - back;
	}
	memcpy(out, (const uint8_t *)source->mv_data + start, MDB_HASH_SIZE);
	return MDB_SUCCESS;
}

static inline size_t
mdb_agg_value_size_flags(uint16_t agg)
{
	size_t n = 0;
	if (agg & MDB_AGG_ENTRIES) n += MDB_AGG_U64_SIZE;
	if (agg & MDB_AGG_KEYS) n += MDB_AGG_U64_SIZE;
	if (agg & MDB_AGG_HASHSUM) n += MDB_HASH_SIZE;
	return n;
}

static inline size_t
mdb_agg_prefix_len_flags(uint16_t agg)
{
	return EVEN(mdb_agg_value_size_flags(agg));
}

static inline size_t
mdb_agg_prefix_bytes(const MDB_page *mp)
{
	return (mp && IS_BRANCH(mp)) ?
		mdb_agg_prefix_len_flags(PAGE_AGGFLAGS(mp)) : 0;
}

static inline void
mdb_node_get_agg_blob(const MDB_page *mp, const MDB_node *node, MDB_aggblob *out)
{
	uint16_t agg = PAGE_AGGFLAGS(mp);
	size_t raw = mdb_agg_value_size_flags(agg);
	if (raw) memcpy(out->v, node->mn_data, raw);
}

static inline void
mdb_node_set_agg_blob(MDB_page *mp, MDB_node *node, const MDB_aggblob *in)
{
	uint16_t agg = PAGE_AGGFLAGS(mp);
	size_t raw = mdb_agg_value_size_flags(agg);
	size_t pfx = mdb_agg_prefix_len_flags(agg);
	if (raw) memcpy(node->mn_data, in->v, raw);
	if (pfx > raw) memset(node->mn_data + raw, 0, pfx - raw);
}


static inline void
mdb_node_get_aggval(const MDB_page *mp, const MDB_node *node, MDB_aggval *out)
{
	MDB_aggblob blob;
	uint16_t agg = PAGE_AGGFLAGS(mp);

	memset(&blob, 0, sizeof(blob));
	mdb_node_get_agg_blob(mp, node, &blob);
	mdb_aggval_from_blob(agg, &blob, out);
}

static inline void
mdb_node_set_aggval(MDB_page *mp, MDB_node *node, const MDB_aggval *in)
{
	MDB_aggblob blob;
	uint16_t agg = PAGE_AGGFLAGS(mp);

	mdb_aggval_to_blob(agg, in, &blob);
	mdb_node_set_agg_blob(mp, node, &blob);
}

static inline void *
mdb_nodekey(const MDB_page *mp, const MDB_node *node)
{
	return (void *)((char *)node->mn_data + mdb_agg_prefix_bytes(mp));
}

