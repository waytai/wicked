/*
 * Interfacing with ModemManager through its dbus interface
 *
 * Copyright (C) 2012 Olaf Kirch <okir@suse.de>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wicked/util.h>
#include <wicked/dbus.h>
#include <wicked/dbus-errors.h>
#include <netinfo_priv.h>
#include <errno.h>
#include <ctype.h>

#include "dbus-client.h"
#include "dbus-dict.h"
#include "dbus-common.h"
#include "dbus-objects/model.h"
#include "modem-manager.h"

#define NI_MM_SIGNAL_DEVICE_ADDED "DeviceAdded"
#define NI_MM_SIGNAL_DEVICE_REMOVED "DeviceRemoved"

#define NI_MM_BUS_NAME		"org.freedesktop.ModemManager"
#define NI_MM_OBJECT_PATH	"/org/freedesktop/ModemManager"
#define NI_MM_INTERFACE		"org.freedesktop.ModemManager"
#define NI_MM_DEV_PATH_PFX	"/org/freedesktop/ModemManager/Modems/"
#define NI_MM_MODEM_IF		"org.freedesktop.ModemManager.Modem"
#define NI_MM_GSM_CARD_IF	"org.freedesktop.ModemManager.Gsm.Card"

typedef struct ni_modem_manager_client ni_modem_manager_client_t;
struct ni_modem_manager_client {
	ni_dbus_client_t *	dbus;

	ni_dbus_object_t *	proxy;
};

static ni_dbus_class_t		ni_objectmodel_modem_manager_class = {
	"modem-manager"
};
#if 0
static ni_dbus_class_t		ni_objectmodel_modem_class = {
	"modem"
};
#endif

static ni_modem_manager_client_t *ni_modem_manager_client;

static void	ni_modem_manager_add_modem(ni_modem_manager_client_t *modem_manager, const char *object_path);
static void	ni_modem_manager_signal(ni_dbus_connection_t *, ni_dbus_message_t *, void *);

ni_modem_manager_client_t *
ni_modem_manager_client_open(void)
{
	ni_dbus_client_t *dbc;
	ni_modem_manager_client_t *modem_manager;

	dbc = ni_dbus_client_open("system", NI_MM_BUS_NAME);
	if (!dbc)
		return NULL;

	/* ni_dbus_client_set_error_map(dbc, __ni_modem_manager_error_names); */

	modem_manager = xcalloc(1, sizeof(*modem_manager));
	modem_manager->proxy = ni_dbus_client_object_new(dbc,
						&ni_objectmodel_modem_manager_class,
						NI_MM_OBJECT_PATH, NI_MM_INTERFACE,
						modem_manager);
	modem_manager->dbus = dbc;

	ni_dbus_client_add_signal_handler(dbc,
				NI_MM_BUS_NAME,		/* sender */
				NULL,			/* object path */
				NI_MM_INTERFACE,	/* object interface */
				ni_modem_manager_signal,
				modem_manager);

	return modem_manager;
}

void
ni_modem_manager_client_free(ni_modem_manager_client_t *modem_manager)
{
	if (modem_manager->dbus) {
		ni_dbus_client_free(modem_manager->dbus);
		modem_manager->dbus = NULL;
	}

	if (modem_manager->proxy) {
		ni_dbus_object_free(modem_manager->proxy);
		modem_manager->proxy = NULL;
	}

	free(modem_manager);
}

ni_bool_t
ni_modem_manager_enumerate(ni_modem_manager_client_t *modem_manager)
{
	DBusError error = DBUS_ERROR_INIT;
	ni_dbus_variant_t resp = NI_DBUS_VARIANT_INIT;
	unsigned int i;
	dbus_bool_t rv;

	rv = ni_dbus_object_call_variant(modem_manager->proxy,
					NI_MM_INTERFACE, "EnumerateDevices",
					0, NULL, 1, &resp, &error);
	if (!rv) {
		ni_dbus_print_error(&error, "unable to enumerate modem devices");
		dbus_error_free(&error);
		return FALSE;
	}

	if (!ni_dbus_variant_is_array_of(&resp, DBUS_TYPE_OBJECT_PATH_AS_STRING)) {
		ni_error("%s: unexpected return value - expected array of object paths, got %s",
				__func__, ni_dbus_variant_signature(&resp));
		rv = FALSE;
		goto done;
	}

	for (i = 0; i < resp.array.len; ++i) {
		const char *object_path = resp.string_array_value[i];

		ni_modem_manager_add_modem(modem_manager, object_path);
	}

done:
	ni_dbus_variant_destroy(&resp);
	return rv;
}

ni_bool_t
ni_modem_manager_init(void)
{
	if (!ni_modem_manager_client) {
		ni_modem_manager_client_t *client;

		client = ni_modem_manager_client_open();
		if (!client)
			return FALSE;

		if (!ni_modem_manager_enumerate(client)) {
			ni_modem_manager_client_free(client);
			return FALSE;
		}

		ni_modem_manager_client = client;
	}

	return TRUE;
}

static void
ni_modem_manager_add_modem(ni_modem_manager_client_t *modem_manager, const char *object_path)
{
	ni_debug_dbus("%s(%s)", __func__, object_path);
}

ni_dbus_client_t *
ni_modem_manager_client_dbus(ni_modem_manager_client_t *modem_manager)
{
	return modem_manager->dbus;
}

static void
ni_modem_manager_signal(ni_dbus_connection_t *conn, ni_dbus_message_t *msg, void *user_data)
{
	ni_modem_manager_client_t *modem_manager = user_data;
	const char *member = dbus_message_get_member(msg);

	ni_debug_dbus("%s: %s", __func__, member);
	if (!strcmp(member, NI_MM_SIGNAL_DEVICE_ADDED)) {
		/* TBD */
	} else
	if (!strcmp(member, NI_MM_SIGNAL_DEVICE_REMOVED)) {
		/* TBD */
	} else {
		ni_debug_wireless("%s signal received (not handled)", member);
	}
	(void) modem_manager;
}