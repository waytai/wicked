/*
 * DBus encapsulation for interface-specific IPv4 settings
 *
 * Copyright (C) 2012 Olaf Kirch <okir@suse.de>
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>

#include <wicked/netinfo.h>
#include <wicked/addrconf.h>
#include <wicked/logging.h>
#include <wicked/dbus-errors.h>
#include <wicked/dbus-service.h>
#include <wicked/system.h>
#include <wicked/ipv4.h>
#include "netinfo_priv.h"
#include "dbus-common.h"
#include "model.h"
#include "debug.h"

static ni_netdev_t *	__ni_objectmodel_protocol_arg(const ni_dbus_variant_t *, const ni_dbus_service_t *);

/*
 * IPv4.changeProtocol method
 */
static dbus_bool_t
ni_objectmodel_ipv4_change_protocol(ni_dbus_object_t *object, const ni_dbus_method_t *method,
			unsigned int argc, const ni_dbus_variant_t *argv,
			ni_dbus_message_t *reply, DBusError *error)
{
	ni_netconfig_t *nc = ni_global_state_handle(0);
	ni_netdev_t *dev, *cfg;
	dbus_bool_t rv = FALSE;

	/* we've already checked that argv matches our signature */
	ni_assert(argc == 1);

	if (!(dev = ni_objectmodel_unwrap_netif(object, error)))
		return FALSE;

	if (!(cfg = __ni_objectmodel_protocol_arg(&argv[0], &ni_objectmodel_ipv4_service))) {
		ni_dbus_error_invalid_args(error, object->path, method->name);
		goto out;
	}

	if (ni_system_ipv4_setup(nc, dev, &cfg->ipv4->conf) < 0) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "failed to set up ethernet device");
		goto out;
	}

	rv = TRUE;

out:
	if (cfg)
		ni_netdev_put(cfg);
	return rv;
}

/*
 * Common helper function to extract bonding device info from a dbus dict
 */
static ni_netdev_t *
__ni_objectmodel_protocol_arg(const ni_dbus_variant_t *dict, const ni_dbus_service_t *service)
{
	ni_dbus_object_t *dev_object;
	ni_netdev_t *dev;
	dbus_bool_t rv;

	dev = ni_netdev_new(NULL, 0);
	dev->link.type = NI_IFTYPE_ETHERNET;

	dev_object = ni_objectmodel_wrap_netif(dev);
	rv = ni_dbus_object_set_properties_from_dict(dev_object, service, dict, NULL);
	ni_dbus_object_free(dev_object);

	if (!rv) {
		ni_netdev_put(dev);
		dev = NULL;
	}
	return dev;
}

/*
 * Functions for dealing with IPv4 properties
 */
static ni_ipv4_devinfo_t *
__ni_objectmodel_ipv4_devinfo_handle(const ni_dbus_object_t *object, ni_bool_t write_access, DBusError *error)
{
	ni_netdev_t *dev;
	ni_ipv4_devinfo_t *ipv4;

	if (!(dev = ni_objectmodel_unwrap_netif(object, error)))
		return NULL;

	if (!write_access)
		return dev->ipv4;

	if (!(ipv4 = ni_netdev_get_ipv4(dev))) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "Unable to get ipv4_devinfo handle for interface");
		return NULL;
	}
	return ipv4;
}

void *
ni_objectmodel_get_ipv4_devinfo(const ni_dbus_object_t *object, ni_bool_t write_access, DBusError *error)
{
	return __ni_objectmodel_ipv4_devinfo_handle(object, write_access, error);
}

#define IPV4_UINT_PROPERTY(dbus_name, member_name, rw) \
	NI_DBUS_GENERIC_UINT_PROPERTY(ipv4_devinfo, dbus_name, member_name, rw)
#define IPV4_BOOL_PROPERTY(dbus_name, member_name, rw) \
	NI_DBUS_GENERIC_BOOL_PROPERTY(ipv4_devinfo, dbus_name, member_name, rw)

const ni_dbus_property_t	ni_objectmodel_ipv4_property_table[] = {
	IPV4_UINT_PROPERTY(forwarding, conf.forwarding, RO),
	IPV4_BOOL_PROPERTY(accept-redirects, conf.accept_redirects, RO),

	{ NULL }
};

static ni_dbus_method_t		ni_objectmodel_ipv4_methods[] = {
	{ "changeProtocol",	"a{sv}",		ni_objectmodel_ipv4_change_protocol },
	{ NULL }
};

ni_dbus_service_t	ni_objectmodel_ipv4_service = {
	.name		= NI_OBJECTMODEL_IPV4_INTERFACE,
	.methods	= ni_objectmodel_ipv4_methods,
	.properties	= ni_objectmodel_ipv4_property_table,
};

