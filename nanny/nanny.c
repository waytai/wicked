/*
 * This daemon manages interface policies.
 *
 * Copyright (C) 2010-2012 Olaf Kirch <okir@suse.de>
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/poll.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>

#include <wicked/netinfo.h>
#include <wicked/addrconf.h>
#include <wicked/logging.h>
#include <wicked/wicked.h>
#include <wicked/socket.h>
#include <wicked/objectmodel.h>
#include <wicked/modem.h>
#include <wicked/dbus-service.h>
#include <wicked/dbus-errors.h>
#include <wicked/fsm.h>
#include "nanny.h"


static void		__ni_nanny_user_free(ni_nanny_user_t *);
static int		ni_nanny_prompt(const ni_fsm_prompt_t *, xml_node_t *, void *);

/*
 * Initialize the manager objectmodel
 */
void
ni_objectmodel_nanny_init(ni_nanny_t *mgr)
{
	ni_dbus_object_t *root_object;

	ni_objectmodel_managed_policy_init(mgr->server);
	ni_objectmodel_managed_netif_init(mgr->server);
	ni_objectmodel_managed_modem_init(mgr->server);

	ni_objectmodel_register_service(&ni_objectmodel_nanny_service);

	root_object = ni_dbus_server_get_root_object(mgr->server);
	root_object->handle = mgr;
	root_object->class = &ni_objectmodel_nanny_class;
	ni_objectmodel_bind_compatible_interfaces(root_object);

	{
		unsigned int i;

		ni_debug_nanny("%s supports:", ni_dbus_object_get_path(root_object));
		for (i = 0; root_object->interfaces[i]; ++i) {
			const ni_dbus_service_t *service = root_object->interfaces[i];

			ni_debug_nanny("  %s", service->name);
		}
		ni_debug_nanny("(%u interfaces)", i);
	}
}

ni_nanny_t *
ni_nanny_new(void)
{
	ni_nanny_t *mgr;

	mgr = calloc(1, sizeof(*mgr));
	return mgr;
}

void
ni_nanny_start(ni_nanny_t *mgr)
{
	ni_nanny_devmatch_t *match;

	mgr->server = ni_server_listen_dbus(NI_OBJECTMODEL_DBUS_BUS_NAME_NANNY);
	if (!mgr->server)
		ni_fatal("Cannot create server, giving up.");

	mgr->fsm = ni_fsm_new();

	ni_fsm_set_user_prompt_fn(mgr->fsm, ni_nanny_prompt, mgr);

	ni_objectmodel_nanny_init(mgr);
	ni_objectmodel_register_all();

	/* Resolve all class references in <enable> config elements,
	 * so that we don't have to do this again for every new device we
	 * discover.
	 */
	for (match = mgr->enable; match; match = match->next) {
		switch (match->type) {
		case NI_NANNY_DEVMATCH_CLASS:
			match->class = ni_objectmodel_get_class(match->value);
			if (!match->class)
				ni_error("cannot enable devices of class \"%s\" - no such class",
						match->value);
			break;
		}
	}
}

void
ni_nanny_free(ni_nanny_t *mgr)
{
	if (mgr->users) {
		ni_nanny_user_t *user;

		while ((user = mgr->users) != NULL) {
			mgr->users = user->next;
			__ni_nanny_user_free(user);
		}
	}

	ni_fatal("%s(): incomplete", __func__);
}

/*
 * (Re-) check a device to see if the applicable policy changed.
 * This is a multi-stage process.
 * One, devices can be scheduled for a recheck explicitly (eg when they
 * appear via hotplug).
 *
 * Two, all enabled devices are checked when policies have been updated.
 *
 * Both checks happen once per mainloop iteration.
 */
void
ni_nanny_schedule_recheck(ni_nanny_t *mgr, ni_ifworker_t *w)
{
	if (ni_ifworker_array_index(&mgr->recheck, w) < 0)
		ni_ifworker_array_append(&mgr->recheck, w);
}

void
ni_nanny_recheck_do(ni_nanny_t *mgr)
{
	unsigned int i;

	if (ni_fsm_policies_changed_since(mgr->fsm, &mgr->last_policy_seq)) {
		ni_managed_device_t *mdev;

		for (mdev = mgr->device_list; mdev; mdev = mdev->next) {
			if (mdev->monitor)
				ni_nanny_schedule_recheck(mgr, mdev->worker);
		}
	}

	if (mgr->recheck.count == 0)
		return;

	ni_fsm_refresh_state(mgr->fsm);

	for (i = 0; i < mgr->recheck.count; ++i)
		ni_nanny_recheck(mgr, mgr->recheck.data[i]);
	ni_ifworker_array_destroy(&mgr->recheck);
}

/*
 * Check whether a given interface should be reconfigured
 */
void
ni_nanny_recheck(ni_nanny_t *mgr, ni_ifworker_t *w)
{
	static const unsigned int MAX_POLICIES = 20;
	const ni_fsm_policy_t *policies[MAX_POLICIES];
	const ni_fsm_policy_t *policy;
	ni_managed_device_t *mdev;
	ni_managed_policy_t *mpolicy;
	unsigned int count;

	/* Ignore devices that went away */
	if (w->dead)
		return;

	if ((mdev = ni_nanny_get_device(mgr, w)) == NULL)
		return;

	/* Note, we also check devices in state FAILED.
	 * ni_managed_device_apply_policy() will then check if the policy
	 * changed. If it did, then we give it another try.
	 */

	ni_debug_nanny("%s(%s)", __func__, w->name);
	w->use_default_policies = TRUE;
	if ((count = ni_fsm_policy_get_applicable_policies(mgr->fsm, w, policies, MAX_POLICIES)) == 0) {
		/* Don't try to take down a FAILED device.
		 * Either we succeed, then we mark it STOPPED (and then try to take it
		 * up again... and fail), or we fail to take it down (and then we try to
		 * take it down once more... and fail).
		 * In either case, we're ending up in a endless loop.
		 * FIXME: use a ni_managed_device_down_emergency() function, which does a hard
		 * shutdown of the device. This needs cooperation from the server; which would have
		 * to kill all leases and destroy all addresses.
		 */
		if (mdev->state != NI_MANAGED_STATE_STOPPED && mdev->state != NI_MANAGED_STATE_FAILED) {
			ni_debug_nanny("%s: taking down device", w->name);
			ni_managed_device_down(mdev);
		} else {
			ni_debug_nanny("%s: no applicable policies", w->name);
		}
		return;
	}

	policy = policies[count-1];
	mpolicy = ni_nanny_get_policy(mgr, policy);

	ni_managed_device_apply_policy(mdev, mpolicy);
}

/*
 * Taking down an interface
 */
void
ni_nanny_schedule_down(ni_nanny_t *mgr, ni_ifworker_t *w)
{
	if (ni_ifworker_array_index(&mgr->down, w) < 0)
		ni_ifworker_array_append(&mgr->down, w);
}

void
ni_nanny_down_do(ni_nanny_t *mgr)
{
	unsigned int i;

	if (mgr->down.count == 0)
		return;

	for (i = 0; i < mgr->down.count; ++i) {
		ni_ifworker_t *w = mgr->down.data[i];
		ni_managed_device_t *mdev;

		if ((mdev = ni_nanny_get_device(mgr, w)) != NULL)
			ni_managed_device_down(mdev);
	}
	ni_ifworker_array_destroy(&mgr->down);
}

ni_managed_policy_t *
ni_nanny_get_policy(ni_nanny_t *mgr, const ni_fsm_policy_t *policy)
{
	ni_managed_policy_t *mpolicy;

	for (mpolicy = mgr->policy_list; mpolicy; mpolicy = mpolicy->next) {
		if (mpolicy->fsm_policy == policy)
			return mpolicy;
	}
	return NULL;
}

/*
 * Handle events from an rfkill switch
 */
void
ni_nanny_rfkill_event(ni_nanny_t *mgr, ni_rfkill_type_t type, ni_bool_t blocked)
{
	ni_managed_device_t *mdev;

	for (mdev = mgr->device_list; mdev; mdev = mdev->next) {
		ni_ifworker_t *w = mdev->worker;

		if (ni_ifworker_get_rfkill_type(w) == type) {
			mdev->rfkill_blocked = blocked;
			if (blocked) {
				ni_debug_nanny("%s: radio disabled", w->name);
			} else {
				/* Re-enable scanning */
				ni_debug_nanny("%s: radio re-enabled, resume monitoring", w->name);
				if (mdev->monitor) {
					ni_managed_netdev_enable(mdev);
					ni_nanny_schedule_recheck(mgr, w);
				}
			}
		}
	}
}

/*
 * Register a device
 */
void
ni_nanny_register_device(ni_nanny_t *mgr, ni_ifworker_t *w)
{
	ni_managed_device_t *mdev;
	const ni_dbus_class_t *dev_class = NULL;
	ni_nanny_devmatch_t *match;

	if (ni_nanny_get_device(mgr, w) != NULL)
		return;

	mdev = ni_managed_device_new(mgr, w, &mgr->device_list);
	if (w->type == NI_IFWORKER_TYPE_NETDEV) {
		mdev->object = ni_objectmodel_register_managed_netdev(mgr->server, mdev);
		dev_class = ni_objectmodel_link_class(w->device->link.type);
	} else
	if (w->type == NI_IFWORKER_TYPE_MODEM) {
		mdev->object = ni_objectmodel_register_managed_modem(mgr->server, mdev);
		dev_class = ni_objectmodel_modem_get_class(w->modem->type);
	}


	for (match = mgr->enable; match; match = match->next) {
		switch (match->type) {
		case NI_NANNY_DEVMATCH_CLASS:
			if (match->class == NULL || dev_class == NULL
			 || !ni_dbus_class_is_subclass(dev_class, match->class))
				continue;
			ni_trace("devmatch class %s: %p", match->value, match->class);
			break;

		case NI_NANNY_DEVMATCH_DEVICE:
			if (!ni_string_eq(w->name, match->value))
				continue;
			break;
		}

		mdev->allowed = TRUE;
		if (match->auto_enable)
			mdev->monitor = TRUE;
	}

	ni_debug_nanny("new device %s, class %s%s%s", w->name,
			mdev->object->class->name,
			mdev->allowed? ", user control allowed" : "",
			mdev->monitor? ", auto-enabled" : "");

	if (mdev->monitor)
		ni_nanny_schedule_recheck(mgr, w);
}

/*
 * Unregister a device
 */
void
ni_nanny_unregister_device(ni_nanny_t *mgr, ni_ifworker_t *w)
{
	ni_managed_device_t *mdev = NULL;

	if ((mdev = ni_nanny_get_device(mgr, w)) == NULL) {
		ni_error("%s: cannot unregister; device not known", w->name);
		return;
	}

	ni_nanny_remove_device(mgr, mdev);
	ni_objectmodel_unregister_managed_device(mdev);
	ni_fsm_destroy_worker(mgr->fsm, w);
}

/*
 * Handle prompting
 */
static ni_ifworker_t *
ni_nanny_identify_node_owner(ni_nanny_t *mgr, xml_node_t *node, ni_stringbuf_t *path)
{
	ni_managed_device_t *mdev;
	ni_ifworker_t *w = NULL;

	for (mdev = mgr->device_list; mdev; mdev = mdev->next) {
		if (mdev->selected_config == node) {
			w = mdev->worker;
			goto found;
		}
	}

	if (node != NULL)
		w = ni_nanny_identify_node_owner(mgr, node->parent, path);

	if (w == NULL)
		return NULL;

found:
	ni_stringbuf_putc(path, '/');
	ni_stringbuf_puts(path, node->name);
	return w;
}

int
ni_nanny_prompt(const ni_fsm_prompt_t *p, xml_node_t *node, void *user_data)
{
	ni_nanny_t *mgr = user_data;
	ni_stringbuf_t path_buf;
	ni_ifworker_t *w = NULL;
	ni_managed_device_t *mdev;
	ni_nanny_user_t *user;
	ni_secret_t *sec;
	int rv = -1;

	ni_debug_nanny("%s: type=%u string=%s id=%s", __func__, p->type, p->string, p->id);

	ni_stringbuf_init(&path_buf);

	w = ni_nanny_identify_node_owner(mgr, node, &path_buf);
	if (w == NULL) {
		ni_error("%s: unable to identify device owning this config", __func__);
		goto done;
	}

	if (!(mdev = ni_nanny_get_device(mgr, w))) {
		ni_error("%s: device not managed by us?!", w->name);
		goto done;
	}

	if (w->security_id.attributes.count == 0) {
		ni_error("%s: no security id set, cannot handle prompt for \"%s\"",
				w->name, path_buf.string);
		goto done;
	}

	if (mdev->selected_policy == NULL) {
		ni_error("%s: no policy set, cannot handle prompt for \"%s\"",
				w->name, path_buf.string);
		goto done;
	}
	if ((user = ni_nanny_get_user(mgr, mdev->selected_policy->owner)) == NULL) {
		ni_error("%s: policy not owned by anyone?!", w->name);
		goto done;
	}

	sec = ni_secret_db_find(user->secret_db, &w->security_id, path_buf.string);
	if (sec == NULL) {
		if (mdev->state == NI_MANAGED_STATE_BINDING) {
			mdev->missing_secrets = TRUE;

			/* Return retry-operation - this makes the validator ignore
			 * the missing secret. In this way, we can record all required
			 * secrets. */
			ni_secret_array_append(&mdev->secrets, sec);
			rv = -NI_ERROR_RETRY_OPERATION;
			goto done;
		}
#if 0
		/* FIXME: Send out event that we need this piece of information */
		ni_debug_nanny("%s: prompting for type=%u id=%s path=%s",
				w->name, p->type, w->security_id, path_buf.string);
#endif
		goto done;
	}

	xml_node_set_cdata(node, sec->value);
	rv = 0;

done:
	ni_stringbuf_destroy(&path_buf);
	return rv;
}

/*
 * Update a secret. If there's a device that has been waiting for this
 * update, recheck it now.
 */
void
ni_nanny_add_secret(ni_nanny_t *mgr, uid_t caller_uid,
			const ni_security_id_t *security_id, const char *path, const char *value)
{
	ni_managed_device_t *mdev;
	ni_nanny_user_t *user;

	if (!(user = ni_nanny_create_user(mgr, caller_uid)))
		return;

	ni_secret_db_update(user->secret_db, security_id, path, value);

	ni_debug_nanny("%s: secret for %s updated", ni_security_id_print(security_id), path);
	for (mdev = mgr->device_list; mdev; mdev = mdev->next) {
		ni_ifworker_t *w = mdev->worker;

		if (mdev->missing_secrets) {
			ni_secret_t *missing = NULL;
			unsigned int i;

			for (i = 0; i < mdev->secrets.count; ++i) {
				ni_secret_t *osec = mdev->secrets.data[i];
				if (osec->value == NULL)
					missing = osec;
			}

			if (missing) {
				ni_debug_nanny("%s: secret for %s still missing", w->name, missing->path);
				continue;
			}

			ni_debug_nanny("%s: secret for %s updated, rechecking", w->name, path);
			ni_nanny_schedule_recheck(mgr, w);
		}
	}
}

void
ni_nanny_clear_secrets(ni_nanny_t *mgr, const ni_security_id_t *security_id, const char *path)
{
	ni_nanny_user_t *user;

	for (user = mgr->users; user; user = user->next)
		ni_secret_db_drop(user->secret_db, security_id, path);
}

/*
 * Handle nanny_user objects
 */
ni_nanny_user_t *
ni_nanny_user_new(uid_t uid)
{
	ni_nanny_user_t *user;

	user = calloc(1, sizeof(*user));
	user->uid = uid;
	user->secret_db = ni_secret_db_new();
	return user;
}

void
__ni_nanny_user_free(ni_nanny_user_t *user)
{
	ni_secret_db_free(user->secret_db);
	user->secret_db = NULL;
	free(user);
}

static ni_nanny_user_t *
__ni_nanny_get_user(ni_nanny_t *mgr, uid_t uid, ni_bool_t create)
{
	ni_nanny_user_t **pos, *user;

	for (pos = &mgr->users; (user = *pos) != NULL; pos = &user->next) {
		if (user->uid == uid)
			return user;
	}

	if (create)
		*pos = user = ni_nanny_user_new(uid);

	return user;
}

ni_nanny_user_t *
ni_nanny_get_user(ni_nanny_t *mgr, uid_t uid)
{
	return __ni_nanny_get_user(mgr, uid, FALSE);
}

ni_nanny_user_t *
ni_nanny_create_user(ni_nanny_t *mgr, uid_t uid)
{
	return __ni_nanny_get_user(mgr, uid, TRUE);
}

/*
 * Extract fsm handle from dbus object
 */
static ni_nanny_t *
ni_objectmodel_nanny_unwrap(const ni_dbus_object_t *object, DBusError *error)
{
	ni_nanny_t *mgr = object->handle;

	if (ni_dbus_object_isa(object, &ni_objectmodel_nanny_class))
		return mgr;

	if (error)
		dbus_set_error(error, DBUS_ERROR_FAILED,
			"method not compatible with object %s of class %s",
			object->path, object->class->name);
	return NULL;
}

/*
 * Nanny.getDevice(devname)
 */
static dbus_bool_t
ni_objectmodel_nanny_get_device(ni_dbus_object_t *object, const ni_dbus_method_t *method,
					unsigned int argc, const ni_dbus_variant_t *argv,
					ni_dbus_message_t *reply, DBusError *error)
{
	ni_nanny_t *mgr;
	const char *ifname;
	ni_ifworker_t *w;
	ni_managed_device_t *mdev = NULL;

	if ((mgr = ni_objectmodel_nanny_unwrap(object, error)) == NULL)
		return FALSE;

	if (argc != 1 || !ni_dbus_variant_get_string(&argv[0], &ifname))
		return ni_dbus_error_invalid_args(error, ni_dbus_object_get_path(object), method->name);

	/* XXX: scalability. Use ni_call_identify_device() */
	ni_fsm_refresh_state(mgr->fsm);
	w = ni_fsm_ifworker_by_name(mgr->fsm, NI_IFWORKER_TYPE_NETDEV, ifname);

	if (w)
		mdev = ni_nanny_get_device(mgr, w);

	if (mdev == NULL) {
		dbus_set_error(error, NI_DBUS_ERROR_DEVICE_NOT_KNOWN, "No such device: %s", ifname);
		return FALSE;
	} else {
		ni_netdev_t *dev = ni_ifworker_get_netdev(w);
		char object_path[128];

		snprintf(object_path, sizeof(object_path),
				NI_OBJECTMODEL_MANAGED_NETIF_LIST_PATH "/%u",
				dev->link.ifindex);

		ni_dbus_message_append_object_path(reply, object_path);
	}
	return TRUE;
}


/*
 * Nanny.createPolicy()
 */
static dbus_bool_t
ni_objectmodel_nanny_create_policy(ni_dbus_object_t *object, const ni_dbus_method_t *method,
					unsigned int argc, const ni_dbus_variant_t *argv,
					ni_dbus_message_t *reply, DBusError *error)
{
	ni_dbus_object_t *policy_object;
	ni_nanny_t *mgr;
	ni_fsm_policy_t *policy;
	const char *name;
	char namebuf[64];

	if ((mgr = ni_objectmodel_nanny_unwrap(object, error)) == NULL)
		return FALSE;

	if (argc != 1 || !ni_dbus_variant_get_string(&argv[0], &name))
		return ni_dbus_error_invalid_args(error, ni_dbus_object_get_path(object), method->name);

	if (*name == '\0') {
		static unsigned int counter = 0;

		do {
			snprintf(namebuf, sizeof(namebuf), "policy%u", counter++);
		} while (ni_fsm_policy_by_name(mgr->fsm, namebuf) == NULL);
		name = namebuf;
	}

#ifdef notyet
	if (!ni_policy_name_valid(name)) {
		dbus_set_error(error, DBUS_ERROR_INVALID_ARGS,
				"Bad policy name \"%s\" in call to %s.%s",
				name, ni_dbus_object_get_path(object), method->name);
		return FALSE;
	}
#endif

	if (ni_fsm_policy_by_name(mgr->fsm, name) != NULL) {
		dbus_set_error(error, NI_DBUS_ERROR_POLICY_EXISTS,
				"Policy \"%s\" already exists in call to %s.%s",
				name, ni_dbus_object_get_path(object), method->name);
		return FALSE;
	}

	policy = ni_fsm_policy_new(mgr->fsm, name, NULL);

	policy_object = ni_objectmodel_register_managed_policy(ni_dbus_object_get_server(object),
					ni_managed_policy_new(mgr, policy, NULL));

	ni_dbus_message_append_object_path(reply, ni_dbus_object_get_path(policy_object));
	return TRUE;
}

/*
 * Nanny.addSecret(security-id, path, value)
 * The security-id is an identifier that is derived from eg the modem's IMEI,
 * or the wireless ESSID.
 */
static dbus_bool_t
ni_objectmodel_nanny_set_secret(ni_dbus_object_t *object, const ni_dbus_method_t *method,
					unsigned int argc, const ni_dbus_variant_t *argv,
					uid_t caller_uid,
					ni_dbus_message_t *reply, DBusError *error)
{
	ni_security_id_t security_id = NI_SECURITY_ID_INIT;
	const char *element_path, *value;
	ni_nanny_t *mgr;

	if ((mgr = ni_objectmodel_nanny_unwrap(object, error)) == NULL)
		return FALSE;

	if (argc != 3
	 || !ni_objectmodel_unmarshal_security_id(&security_id, &argv[0])
	 || !ni_dbus_variant_get_string(&argv[1], &element_path)
	 || !ni_dbus_variant_get_string(&argv[2], &value)) {
		ni_security_id_destroy(&security_id);
		return ni_dbus_error_invalid_args(error, ni_dbus_object_get_path(object), method->name);
	}

	ni_nanny_add_secret(mgr, caller_uid, &security_id, element_path, value);
	ni_security_id_destroy(&security_id);
	return TRUE;
}

static ni_dbus_method_t		ni_objectmodel_nanny_methods[] = {
	{ "getDevice",		"s",		ni_objectmodel_nanny_get_device	},
	{ "createPolicy",	"s",		ni_objectmodel_nanny_create_policy	},
	{ "addSecret",		"a{sv}ss",	.handler_ex = ni_objectmodel_nanny_set_secret	},
	{ NULL }
};

ni_dbus_class_t			ni_objectmodel_nanny_class = {
	.name		= "nanny",
};

ni_dbus_service_t		ni_objectmodel_nanny_service = {
	.name		= NI_OBJECTMODEL_NANNY_INTERFACE,
	.compatible	= &ni_objectmodel_nanny_class,
	.methods	= ni_objectmodel_nanny_methods
};
