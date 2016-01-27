/*
 * LUKS - Linux Unified Key Setup v2, digest handling
 *
 * Copyright (C) 2015-2016, Red Hat, Inc. All rights reserved.
 * Copyright (C) 2015-2016, Milan Broz. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "luks2_internal.h"

#define LUKS2_DIGEST_MAX 8

extern const digest_handler LUKS1_digest;

static const digest_handler *digest_handlers[LUKS2_DIGEST_MAX] = {
	&LUKS1_digest,
	NULL
};

int crypt_digest_register(const digest_handler *handler)
{
	int i;

	for (i = 0; i < LUKS2_DIGEST_MAX && digest_handlers[i]; i++) {
		if (!strcmp(digest_handlers[i]->name, handler->name))
			return -EINVAL;
	}

	if (i == LUKS2_DIGEST_MAX)
		return -EINVAL;

	digest_handlers[i] = handler;
	return 0;
}

const digest_handler *LUKS2_digest_handler_type(struct crypt_device *cd, const char *type)
{
	int i;

	for (i = 0; i < LUKS2_DIGEST_MAX && digest_handlers[i]; i++) {
		if (!strcmp(digest_handlers[i]->name, type))
			return digest_handlers[i];
	}

	return NULL;
}

static const digest_handler *LUKS2_digest_handler(struct crypt_device *cd, int digest)
{
	struct luks2_hdr *hdr;
	json_object *jobj1, *jobj2;

	if (digest < 0)
		return NULL;

	if (!(hdr = crypt_get_hdr(cd, CRYPT_LUKS2)))
		return NULL;

	if (!(jobj1 = LUKS2_get_digest_jobj(hdr, digest)))
		return NULL;

	if (!json_object_object_get_ex(jobj1, "type", &jobj2))
		return NULL;

	return LUKS2_digest_handler_type(cd, json_object_get_string(jobj2));
}

int LUKS2_digest_verify(struct crypt_device *cd,
	struct luks2_hdr *hdr,
	struct volume_key *vk,
	int keyslot)
{
	json_object *jobj1, *jobj2, *jobj3;
	char keyslot_name[16];
	const digest_handler *h;
	int i, r, digest;

	/* FIXME: verification without active keyslot, should it be ignored? */
	if (keyslot == CRYPT_ANY_SLOT)
		keyslot = 0;

	if (snprintf(keyslot_name, sizeof(keyslot_name), "%u", keyslot) < 1)
		return -ENOMEM;

	json_object_object_get_ex(hdr->jobj, "digests", &jobj1);

	json_object_object_foreach(jobj1, key, val) {
		json_object_object_get_ex(val, "keyslots", &jobj2);
		for (i = 0; i < json_object_array_length(jobj2); i++) {
			jobj3 = json_object_array_get_idx(jobj2, i);
			if (!strcmp(keyslot_name, json_object_get_string(jobj3))) {
				digest = atoi(key);
				log_dbg("Verifying key from keyslot %d, digest %d.",
					keyslot, digest);
				h = LUKS2_digest_handler(cd, digest);
				if (!h)
					return -EINVAL;

				r = h->verify(cd, digest, vk->key, vk->keylength);
				if (r < 0) {
					log_dbg("Digest %d (%s) verify failed with %d.",
						digest, h->name, r);
					return r;
				}
			}
		}
	}

	return 0;
}

int LUKS2_digest_dump(struct crypt_device *cd, int digest)
{
	const digest_handler *h;

	if (!(h = LUKS2_digest_handler(cd, digest)))
		return -EINVAL;

	return h->dump(cd, digest);
}

int LUKS2_digest_json_get(struct crypt_device *cd, struct luks2_hdr *hdr,
			  int digest, const char **json)
{
	json_object *jobj_digest;

	jobj_digest = LUKS2_get_digest_jobj(hdr, digest);
	if (!jobj_digest)
		return -EINVAL;

	*json = json_object_to_json_string_ext(jobj_digest, JSON_C_TO_STRING_PLAIN);
	return 0;
}

static int assign_one_digest(struct crypt_device *cd, struct luks2_hdr *hdr,
			     int keyslot, int digest, int assign)
{
	json_object *jobj1, *jobj_digest, *jobj_digest_keyslots;
	char num[16];

	log_dbg("Keyslot %i %s digest %i.", keyslot, assign ? "assigned to" : "unassigned from", digest);

	jobj_digest = LUKS2_get_digest_jobj(hdr, digest);
	if (!jobj_digest)
		return -EINVAL;

	json_object_object_get_ex(jobj_digest, "keyslots", &jobj_digest_keyslots);
	if (!jobj_digest_keyslots)
		return -EINVAL;

	snprintf(num, sizeof(num), "%d", keyslot);
	if (assign) {
		jobj1 = LUKS2_array_jobj(jobj_digest_keyslots, num);
		if (!jobj1)
			json_object_array_add(jobj_digest_keyslots, json_object_new_string(num));
	} else {
		jobj1 = LUKS2_array_remove(jobj_digest_keyslots, num);
		if (jobj1)
			json_object_object_add(jobj_digest, "keyslots", jobj1);
	}

	return 0;
}

int LUKS2_digest_assign(struct crypt_device *cd, struct luks2_hdr *hdr,
			int keyslot, int digest, int assign, int commit)
{
	json_object *jobj_digests;
	int r = 0;

	if (!LUKS2_get_keyslot_jobj(hdr, keyslot))
		return -EINVAL;

	if (digest == CRYPT_ANY_DIGEST) {
		json_object_object_get_ex(hdr->jobj, "digests", &jobj_digests);

		json_object_object_foreach(jobj_digests, key, val) {
			UNUSED(val);
			r = assign_one_digest(cd, hdr, keyslot, atoi(key), assign);
			if (r < 0)
				break;
		}
	} else
		r = assign_one_digest(cd, hdr, keyslot, digest, assign);

	if (r < 0)
		return r;

	// FIXME: do not write header in nothing changed
	return commit ? LUKS2_hdr_write(cd, hdr) : 0;
}
