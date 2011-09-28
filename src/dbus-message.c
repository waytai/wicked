/*
 * Convenience functions for marshalling dbus messages
 *
 * Copyright (C) 2011 Olaf Kirch <okir@suse.de>
 */

#include <wicked/util.h>
#include <wicked/dbus.h>

#include "netinfo_priv.h"
#include "dbus-common.h"
#include "dbus-dict.h"

#define TRACE_ENTER()	ni_debug_dbus("%s()", __FUNCTION__)
#define TRACE_ENTERN(fmt, args...) \
			ni_debug_dbus("%s(" fmt ")", __FUNCTION__, ##args)
#define TP()		ni_debug_dbus("TP - %s:%u", __FUNCTION__, __LINE__)


static dbus_bool_t	ni_dbus_message_iter_get_array(DBusMessageIter *, ni_dbus_variant_t *);

dbus_bool_t
ni_dbus_message_iter_append_byte_array(DBusMessageIter *iter,
				const unsigned char *value, unsigned int len)
{
	DBusMessageIter iter_array;
	unsigned int i;

	if (!dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					      DBUS_TYPE_BYTE_AS_STRING,
					      &iter_array))
		return FALSE;

	for (i = 0; i < len; i++) {
		if (!dbus_message_iter_append_basic(&iter_array,
						    DBUS_TYPE_BYTE,
						    &(value[i])))
			return FALSE;
	}

	if (!dbus_message_iter_close_container(iter, &iter_array))
		return FALSE;

	return TRUE;
}

dbus_bool_t
ni_dbus_message_iter_append_string_array(DBusMessageIter *iter,
				char **string_array, unsigned int len)
{
	DBusMessageIter iter_array;
	unsigned int i;

	if (!dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					      DBUS_TYPE_STRING_AS_STRING,
					      &iter_array))
		return FALSE;

	for (i = 0; i < len; i++) {
		if (!dbus_message_iter_append_basic(&iter_array,
						    DBUS_TYPE_STRING,
						    &string_array[i]))
			return FALSE;
	}

	if (!dbus_message_iter_close_container(iter, &iter_array))
		return FALSE;

	return TRUE;
}

dbus_bool_t
ni_dbus_message_iter_append_dict_entry(DBusMessageIter *iter,
				const ni_dbus_dict_entry_t *entry)
{
	DBusMessageIter iter_dict_entry;

	if (!dbus_message_iter_open_container(iter,
					      DBUS_TYPE_DICT_ENTRY, NULL,
					      &iter_dict_entry))
		return FALSE;

	if (!dbus_message_iter_append_basic(&iter_dict_entry, DBUS_TYPE_STRING,
					    &entry->key))
		return FALSE;

	if (!ni_dbus_message_iter_append_variant(&iter_dict_entry, &entry->datum))
		return FALSE;

	if (!dbus_message_iter_close_container(iter, &iter_dict_entry))
		return FALSE;

	return TRUE;
}

dbus_bool_t
ni_dbus_message_iter_append_dict(DBusMessageIter *iter,
				const ni_dbus_dict_entry_t *dict_array, unsigned int len)
{
	DBusMessageIter iter_array;
	unsigned int i;

	if (!dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					      DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					      DBUS_TYPE_STRING_AS_STRING
					      DBUS_TYPE_VARIANT_AS_STRING
					      DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
					      &iter_array))
		return FALSE;

	for (i = 0; i < len; i++) {
		if (!ni_dbus_message_iter_append_dict_entry(&iter_array,
							&dict_array[i]))
			return FALSE;
	}

	if (!dbus_message_iter_close_container(iter, &iter_array))
		return FALSE;

	return TRUE;
}

dbus_bool_t
ni_dbus_message_iter_append_variant_array(DBusMessageIter *iter,
				const ni_dbus_variant_t *variant_array, unsigned int len)
{
	DBusMessageIter iter_array;
	unsigned int i;
	dbus_bool_t rv = TRUE;

	if (!dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					      DBUS_TYPE_VARIANT_AS_STRING,
					      &iter_array))
		return FALSE;

	for (i = 0; rv && i < len; i++) {
		rv = ni_dbus_message_iter_append_variant(&iter_array,
						    &variant_array[i]);
	}

	if (!dbus_message_iter_close_container(iter, &iter_array))
		rv = FALSE;

	return rv;
}

dbus_bool_t
ni_dbus_message_iter_append_some_array(DBusMessageIter *iter,
				const char *element_signature,
				const ni_dbus_variant_t *values,
				unsigned int len)
{
	DBusMessageIter iter_array;
	unsigned int i;
	dbus_bool_t rv = TRUE;

	if (!dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					      element_signature,
					      &iter_array))
		return FALSE;

	for (i = 0; rv && i < len; i++) {
		rv = ni_dbus_message_iter_append_value(&iter_array,
						    &values[i],
						    element_signature);
	}

	if (!dbus_message_iter_close_container(iter, &iter_array))
		rv = FALSE;

	return rv;
}

dbus_bool_t
ni_dbus_message_iter_append_value(DBusMessageIter *iter, const ni_dbus_variant_t *variant, const char *signature)
{
	const void *value;
	DBusMessageIter *iter_val, _iter_val;
	dbus_bool_t rv = FALSE;

	iter_val = iter;
	if (signature == NULL) {
		if (!(signature = ni_dbus_variant_signature(variant)))
			return FALSE;
	} else
	if (signature[0] == DBUS_TYPE_VARIANT) {
		if (!(signature = ni_dbus_variant_signature(variant)))
			return FALSE;
		if (!dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, signature, &_iter_val))
			return FALSE;
		iter_val = &_iter_val;
	}

	value = ni_dbus_variant_datum_const_ptr(variant);
	if (value != NULL) {
		rv = dbus_message_iter_append_basic(iter_val, variant->type, value);
	} else
	if (variant->type == DBUS_TYPE_ARRAY) {
		switch (variant->array.element_type) {
		case DBUS_TYPE_BYTE:
			rv = ni_dbus_message_iter_append_byte_array(iter_val,
					variant->byte_array_value, variant->array.len);
			break;

		case DBUS_TYPE_STRING:
			rv = ni_dbus_message_iter_append_string_array(iter_val,
					variant->string_array_value, variant->array.len);
			break;

		case DBUS_TYPE_VARIANT:
			rv = ni_dbus_message_iter_append_variant_array(iter_val,
					variant->variant_array_value, variant->array.len);
			break;

		case DBUS_TYPE_DICT_ENTRY:
			rv = ni_dbus_message_iter_append_dict(iter_val,
					variant->dict_array_value, variant->array.len);
			break;

		case DBUS_TYPE_INVALID:
			rv = ni_dbus_message_iter_append_some_array(iter_val,
					variant->array.element_signature,
					variant->variant_array_value,
					variant->array.len);
			break;

		default:
			ni_warn("%s: variant type %s not supported", __FUNCTION__, signature);
		}
	} else {
		ni_warn("%s: variant type %s not supported", __FUNCTION__, signature);
	}

	if (iter_val != iter && !dbus_message_iter_close_container(iter, iter_val))
		rv = FALSE;

	return rv;
}

dbus_bool_t
ni_dbus_message_iter_append_variant(DBusMessageIter *iter, const ni_dbus_variant_t *variant)
{
	return ni_dbus_message_iter_append_value(iter, variant, DBUS_TYPE_VARIANT_AS_STRING);
}

dbus_bool_t
ni_dbus_message_iter_get_byte_array(DBusMessageIter *iter, ni_dbus_variant_t *variant)
{
	ni_dbus_variant_init_byte_array(variant);

	while (dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_BYTE) {
		unsigned char byte;

		dbus_message_iter_get_basic(iter, &byte);
		ni_dbus_variant_append_byte_array(variant, byte);
		dbus_message_iter_next(iter);
	}

	return TRUE;
}

dbus_bool_t
ni_dbus_message_iter_get_string_array(DBusMessageIter *iter, ni_dbus_variant_t *variant)
{
	ni_dbus_variant_init_string_array(variant);
	while (dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_STRING) {
		const char *value;

		dbus_message_iter_get_basic(iter, &value);
		ni_dbus_variant_append_string_array(variant, value);
		dbus_message_iter_next(iter);
	}

	return TRUE;
}

dbus_bool_t
ni_dbus_message_iter_get_array_array(DBusMessageIter *iter, ni_dbus_variant_t *variant)
{
	dbus_bool_t rv = TRUE;

	ni_dbus_array_array_init(variant, 
			dbus_message_iter_get_signature(iter));

	while (rv && dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_ARRAY) {
		ni_dbus_variant_t *elem;

		elem = ni_dbus_array_array_add(variant);
		rv = ni_dbus_message_iter_get_array(iter, elem);
		dbus_message_iter_next(iter);
	}

	return rv;
}

dbus_bool_t
ni_dbus_message_iter_get_dict(DBusMessageIter *iter, ni_dbus_variant_t *result)
{
	DBusMessageIter iter_dict;

	ni_dbus_variant_init_dict(result);

	if (!ni_dbus_dict_open_read(iter, &iter_dict))
		return FALSE;

	while (1) {
		ni_dbus_dict_entry_t entry;
		ni_dbus_variant_t *ev;

		memset(&entry, 0, sizeof(entry));
		if (!ni_dbus_dict_get_entry(&iter_dict, &entry))
			break;

		ev = ni_dbus_dict_add(result, entry.key);
		*ev = entry.datum;
	}

	return TRUE;
}


static dbus_bool_t
ni_dbus_message_iter_get_array(DBusMessageIter *iter, ni_dbus_variant_t *variant)
{
	int array_type = dbus_message_iter_get_element_type(iter);
	dbus_bool_t success = FALSE;
	DBusMessageIter iter_array;

	if (!variant)
		return FALSE;

	dbus_message_iter_recurse(iter, &iter_array);

	switch (array_type) {
	case DBUS_TYPE_BYTE:
		success = ni_dbus_message_iter_get_byte_array(&iter_array, variant);
		break;
	case DBUS_TYPE_STRING:
		success = ni_dbus_message_iter_get_string_array(&iter_array, variant);
		break;
	case DBUS_TYPE_DICT_ENTRY:
		success = ni_dbus_message_iter_get_dict(iter, variant);
		break;
	case DBUS_TYPE_ARRAY:
		success = ni_dbus_message_iter_get_array_array(&iter_array, variant);
		break;
	default:
		ni_debug_dbus("%s: cannot decode array of type %c", __FUNCTION__, array_type);
		break;
	}

	return success;
}


dbus_bool_t
ni_dbus_message_iter_get_variant_data(DBusMessageIter *iter, ni_dbus_variant_t *variant)
{
	void *value;

	ni_dbus_variant_destroy(variant);
	variant->type = dbus_message_iter_get_arg_type(iter);

	value = ni_dbus_variant_datum_ptr(variant);
	if (value != NULL) {
		/* Basic types */
		dbus_message_iter_get_basic(iter, value);

		if (variant->type == DBUS_TYPE_STRING
		 || variant->type == DBUS_TYPE_OBJECT_PATH)
			variant->string_value = xstrdup(variant->string_value);
	} else if (variant->type == DBUS_TYPE_ARRAY) {
		if (!ni_dbus_message_iter_get_array(iter, variant))
			return FALSE;
	} else {
		/* FIXME: need to handle other types here */
		return FALSE;
	}

	return TRUE;
}

dbus_bool_t
ni_dbus_message_iter_get_variant(DBusMessageIter *iter, ni_dbus_variant_t *variant)
{
	DBusMessageIter iter_val;
	int type;

	ni_dbus_variant_destroy(variant);

	type = dbus_message_iter_get_arg_type(iter);
	if (type != DBUS_TYPE_VARIANT)
		return FALSE;

	dbus_message_iter_recurse(iter, &iter_val);
	return ni_dbus_message_iter_get_variant_data(&iter_val, variant);
}

