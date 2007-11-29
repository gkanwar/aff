#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <float.h>
#include <limits.h>
#include <stdarg.h>
#include "node.h"
#include "stable.h"
#include "tree.h"
#include "md5.h"
#include "coding.h"
#include "io.h"
#include "aff-i.h"

static void
unpack1(struct AffReader_s *aff, struct RSection_s *section,
       uint8_t buf[AFF_HEADER_SIZE1], int off,
       const char *error_msg)
{
    uint32_t size = AFF_HEADER_SIZE1 - off;
    uint8_t *start = buf + off;
    uint8_t *ptr;
    
    if (aff->error)
	return;

    ptr = aff_decode_u64(&section->start, start, size);
    if (ptr == 0)
	goto error;
    ptr = aff_decode_u64(&section->size, ptr, size - (ptr - start));
    if (ptr == 0)
	goto error;
    if (AFF_HEADER_SIZE1 - (buf - ptr) < 16)
	goto error;
    memcpy(&section->md5, ptr, 16);

    return;
error:
    aff->error = error_msg;
}

static void
unpack2(struct AffReader_s *aff, struct RSection_s *section,
       uint8_t buf[AFF_HEADER_SIZE2], int off,
       const char *error_msg)
{
    uint32_t size = AFF_HEADER_SIZE2 - off;
    uint8_t *start = buf + off;
    uint8_t *ptr;
    
    if (aff->error)
	return;

    ptr = aff_decode_u64(&section->start, start, size);
    if (ptr == 0)
	goto error;
    ptr = aff_decode_u64(&section->size, ptr, size - (ptr - start));
    if (ptr == 0)
	goto error;
    ptr = aff_decode_u32(&section->records, ptr, size - (ptr - start));
    if (ptr == 0)
	goto error;
    if (AFF_HEADER_SIZE2 - (buf - ptr) < 16)
	goto error;
    memcpy(&section->md5, ptr, 16);

    return;
error:
    aff->error = error_msg;
}

static int
read_sig(struct AffReader_s *aff,
	 const uint8_t sig[AFF_SIG_SIZE],
	 const uint8_t *aff_sig)
{
    uint32_t d_exp;
    
    if (memcmp(sig, aff_sig, AFF_SIG_ID_SIZE) != 0) {
	aff->error = "AFF signature mismatch";
	return 1;
    }

    if (sig[AFF_SIG_OFF_DBITS] != sizeof (double) * CHAR_BIT) {
	aff->error = "AFF size of double mismatch";
	return 1;
    }
    if (sig[AFF_SIG_OFF_RADIX] != FLT_RADIX || FLT_RADIX != 2) {
	aff->error = "AFF double radix mismatch";
	return 1;
    }
    if (sig[AFF_SIG_OFF_MANT] != DBL_MANT_DIG) {
	aff->error = "AFF double mantissa size mismatch";
	return 1;
    }

    if (aff_decode_u32(&d_exp,
		       (uint8_t *)sig + AFF_SIG_OFF_EXP,
		       AFF_SIG_SIZE - AFF_SIG_OFF_EXP) == 0) {
	aff->error = "AFF error decoding double exponent sizes";
	return 1;
    }
    if ((d_exp >> 16 != DBL_MAX_EXP) || ((d_exp & 0xffff) != (-DBL_MIN_EXP))) {
	aff->error = "AFF exponent limits mismatch";
	return 1;
    }
    return 0;
}

static int
read_header1(struct AffReader_s *aff,
	     const uint8_t sig[AFF_SIG_SIZE])
{
    struct AffMD5_s md5;
    uint8_t md5_read[16];
    uint8_t buf[AFF_HEADER_SIZE1 - AFF_SIG_SIZE];

    if (read_sig(aff, sig, aff_signature1))
	return 1;

    if (fread(buf, sizeof (buf), 1, aff->file) != 1) {
	aff->error = "Reading V1 header failed";
	return 1;
    }

    aff_md5_init(&md5);
    aff_md5_update(&md5, sig, AFF_SIG_SIZE);
    aff_md5_update(&md5, buf, sizeof (buf) - 16);
    aff_md5_final(md5_read, &md5);
    if (memcmp(md5_read, buf + sizeof (buf) - 16, 16) != 0) {
	aff->error = "V1 header checksum failed";
	return 1;
    }
    
    unpack1(aff, &aff->data_hdr,   buf,  0, "V1 data header unpack failed");
    unpack1(aff, &aff->stable_hdr, buf, 32, "V1 stable header unpack failed");
    unpack1(aff, &aff->tree_hdr,   buf, 64, "V1 tree header unpack failed");
    return aff->error? 1: 0;
}

static int
read_header2(struct AffReader_s *aff,
	     const uint8_t sig[AFF_SIG_SIZE])
{
    struct AffMD5_s md5;
    uint8_t md5_read[16];
    uint8_t buf[AFF_HEADER_SIZE2 - AFF_SIG_SIZE];

    if (read_sig(aff, sig, aff_signature2))
	return 1;

    if (fread(buf, sizeof (buf), 1, aff->file) != 1) {
	aff->error = "Reading V2 header failed";
	return 1;
    }

    aff_md5_init(&md5);
    aff_md5_update(&md5, sig, AFF_SIG_SIZE);
    aff_md5_update(&md5, buf, sizeof (buf) - 16);
    aff_md5_final(md5_read, &md5);
    if (memcmp(md5_read, buf + sizeof (buf) - 16, 16) != 0) {
	aff->error = "V2 header checksum failed";
	return 1;
    }
    
    unpack2(aff, &aff->data_hdr,   buf,  0, "V2 data header unpack failed");
    unpack2(aff, &aff->stable_hdr, buf, 36, "V2 stable header unpack failed");
    unpack2(aff, &aff->tree_hdr,   buf, 72, "V2 tree header unpack failed");
    return aff->error? 1: 0;
}

struct AffReader_s *
aff_reader(const char *file_name)
{
    struct AffReader_s *aff = malloc(sizeof (struct AffReader_s));
    struct AffMD5_s md5;
    uint8_t file_sig[AFF_SIG_SIZE];
    uint8_t md5_read[16];
    uint8_t *sb = 0;
    uint8_t *sym = 0;
    uint32_t rec_count;
    uint32_t byte_count;
    uint64_t tree_size;
    uint32_t sig_size;
#if 0    
    uint32_t data;
    int size;
    int sb_size;
    int sb_used;
    uint64_t string_size;
#endif    

    if (aff == 0)
	return 0;

    memset(aff, 0, sizeof (struct AffReader_s));

    aff->file = fopen(file_name, "rb");
    if (aff->file == 0) {
	aff->error = strerror(errno);
	return aff;
    }
    if (fread(file_sig, AFF_SIG_SIZE, 1, aff->file) != 1) {
	aff->error = "Reading AFF signature failed";
	goto error;
    }
    if (aff_decode_u32(&sig_size,
		       file_sig + AFF_SIG_OFF_SIZE,
		       AFF_SIG_SIZE - AFF_SIG_OFF_SIZE) == 0) {
	aff->error = "AFF signature size decoding failed";
	goto error;
    }

    switch (sig_size) {
    case AFF_HEADER_SIZE1:
	if (read_header1(aff, file_sig))
	    goto error;
	break;
    case AFF_HEADER_SIZE2:
	if (read_header2(aff, file_sig))
	    goto error;
	break;
    default:
	aff->error = "Bad AFF header";
	goto error;
    }
#if 0
    if (fread(buf, sizeof (buf), 1, aff->file) != 1) {
	aff->error = "Reading AFF header failed";
	goto error;
    }
    aff_md5_init(&md5);
    aff_md5_update(&md5, buf, sizeof (buf) - 16);
    aff_md5_final(md5_read, &md5);
    if (memcmp(md5_read, buf + sizeof (buf) - 16, 16) != 0) {
	aff->error = "AFF header failed checksum";
	goto error;
    }

    /* NB: This should match writer_fini() */
    buf[sizeof(buf) - 1] = 0; /* Paranoia? I don't think so */
    if (strcmp((const char *)buf, (const char *)aff_signature) != 0) {
	aff->error = "AFF signature mismatch";
	goto error;
    }
    size = strlen((const char *)aff_signature) + 1;
    if (buf[size] != sizeof (double) * CHAR_BIT) {
	aff->error = "AFF size if double mismatch";
	goto error;
    }
    if (buf[size+1] != FLT_RADIX || FLT_RADIX != 2) {
	aff->error = "AFF double radix mismatch";
	goto error;
    }
    if (buf[size+2] != DBL_MANT_DIG) {
	aff->error = "AFF double mantissa size mismatch";
	goto error;
    }
    if (aff_decode_u32(&data, buf + size + 3, 4) == 0) {
	aff->error = "AFF error decoding double exponent sizes";
	goto error;
    }
    if ((data >> 16 != DBL_MAX_EXP) || ((data & 0xffff) != (- DBL_MIN_EXP))) {
	aff->error = "AFF exponent limits mismatch";
	goto error;
    }
    if (aff_decode_u32(&data, buf + size + 7, 4) == 0 ||
	data != AFF_HEADER_SIZE) {
	aff->error = "AFF header size check failed";
	goto error;
    }
    unpack(aff, &aff->data_hdr,   buf, size+11, "data header unpack failed");
    unpack(aff, &aff->stable_hdr, buf, size+43, "stable header unpack failed");
    unpack(aff, &aff->tree_hdr,   buf, size+75, "tree header unpack failed");

    if (aff->error)
	goto error;
#endif

    rec_count = aff->stable_hdr.records; /* = 0 in V1 */
    byte_count = (uint32_t)aff->stable_hdr.size;
    if (byte_count != aff->stable_hdr.size) {
	aff->error = "Stable too large";
	goto error;
    }
    aff->stable = aff_stable_init(rec_count);
    if (aff->stable == 0) {
	aff->error = "Not enough memory for stable";
	goto error;
    }

    /* load the stable */
    if (aff_file_setpos(aff->file, aff->stable_hdr.start) != 0) {
	aff->error = "Positioning on the string table failed";
	goto error;
    }
    sb = malloc(byte_count);
    if (sb == 0) {
	aff->error = "Not enough memory for the stable data";
	goto error;
    }
    if (fread(sb, byte_count, 1, aff->file) != 1) {
	aff->error = "Error reading stable";
	goto error;
    }
    aff_md5_init(&md5);
    aff_md5_update(&md5, sb, byte_count);
    aff_md5_final(md5_read, &md5);
    if (memcmp(md5_read, aff->stable_hdr.md5, 16) != 0) {
	aff->error = "Stable checksum mismatch";
	goto error;
    }
    if (sb[byte_count - 1] != 0) {
	aff->error = "Malformed stable";
	goto error;
    }

    /* can not use rec_count here */
    for (sym = sb; byte_count;) {
	int slen = strlen((const char *)sym);

	if (aff_stable_insert(aff->stable, (const char *)sym) == 0) {
	    aff->error = "Stable construction error";
	    goto error;
	}
	sym += slen + 1;
	byte_count -= slen + 1;
    }
    free(sb);
    sb = 0;

    /* load the tree */
    rec_count = (uint32_t)(aff->tree_hdr.records);
    if (rec_count != (uint32_t)(aff->tree_hdr.records)) {
	aff->error = "AFF tree too large";
	goto error;
    }
    aff->tree = aff_tree_init(aff->stable, rec_count);
    if (aff->tree == 0) {
	aff->error = "Not enough memory for tree";
	goto error;
    }

    aff_md5_init(&md5);
    for (tree_size = aff->tree_hdr.size; tree_size >= 1 + 8 + 4; ) {
	const struct AffSymbol_s *n_name;
	struct AffNode_s *n_parent;
	struct AffNode_s *node;
	enum AffNodeType_e n_type;
	uint8_t tnode[1 + 8 + 4 + 4 + 8];
	uint64_t f_parent;
	uint32_t f_name;
	uint64_t f_offset;
	uint32_t f_size;

	if (fread(tnode, 1 + 8 + 4, 1, aff->file) != 1) {
	    aff->error = "Tree node reading error";
	    goto error;
	}
	aff_md5_update(&md5, tnode, 1 + 8 + 4);
	tree_size -= 1 + 8 + 4;
	if (aff_decode_type(&n_type, tnode, 1) == 0 ||
	    aff_decode_u64(&f_parent, tnode + 1, 8) == 0 ||
	    aff_decode_u32(&f_name, tnode + 9, 4) == 0) {
	    aff->error = "Error decoding the node record";
	    goto error;
	}
	n_parent = aff_tree_index(aff->tree, f_parent);
	if (n_parent == 0) {
	    aff->error = "Broken tree: missing parent";
	    goto error;
	}
	n_name = aff_stable_index(aff->stable, f_name);
	if (n_name == 0) {
	    aff->error = "Broken tree: missing name";
	    goto error;
	}
	if (n_type != affNodeVoid) {
	    if (tree_size < 4 + 8) {
		aff->error = "Malformed tree data";
		goto error;
	    }
	    if (fread(tnode, 4 + 8, 1, aff->file) != 1) {
		aff->error = "Tree node data reading error";
		goto error;
	    }
	    aff_md5_update(&md5, tnode, 4 + 8);
	    tree_size -= 4 + 8;
	    if (aff_decode_u32(&f_size, tnode, 4) == 0 ||
		aff_decode_u64(&f_offset, tnode + 4, 8) == 0) {
		aff->error = "Tree node data decoding error";
		goto error;
	    }
	} else {
	    f_size = 0;
	    f_offset = 0;
	}
	node = aff_node_chdir(aff->tree, aff->stable,
			      n_parent, 1,
			      aff_symbol_name(n_name));
	if (node == 0) {
	    aff->error = "Node rebuilding error";
	    goto error;
	}
	if (aff_node_assign(node, n_type, f_size, f_offset) != 0) {
	    aff->error = "Node asignment error";
	    goto error;
	}
    }
    if (tree_size != 0) {
	aff->error = "Mismatch in the tree size";
	goto error;
    }
    aff_md5_final(md5_read, &md5);
    if (memcmp(md5_read, aff->tree_hdr.md5, 16) != 0) {
	aff->error = "Tree table checksum mismatch";
	goto error;
    }

    return aff;
error:
    if (sb)
	free(sb);
    sb = 0;
    if (aff->stable)
	aff_stable_fini(aff->stable);
    aff->stable = 0;
    if (aff->tree)
	aff_tree_fini(aff->tree);
    aff->tree = 0;
    fclose(aff->file);
    aff->file = 0;
    return aff;
}
