/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2007  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <assert.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <glib.h>

#include <dbus/dbus.h>

#include "dbus.h"
#include "dbus-helper.h"
#include "logging.h"

#include "headset.h"
#include "manager.h"

typedef enum {
	HEADSET	= 1 << 0,
	GATEWAY	= 1 << 1,
	SINK	= 1 << 2,
	SOURCE	= 1 << 3,
	CONTROL	= 1 << 4,
	TARGET	= 1 << 5,
	INVALID	= 1 << 6
} audio_service_type;

typedef enum {
		GENERIC_AUDIO = 0,
		ADVANCED_AUDIO,
		AV_REMOTE,
		GET_RECORDS
} audio_sdp_state_t;

struct audio_sdp_data {
	audio_device_t *device;

	DBusMessage *msg;	/* Method call or NULL */

	GSList *handles;	/* uint32_t * */
	GSList *records;	/* sdp_record_t * */

	audio_sdp_state_t state;
};

static DBusConnection *connection = NULL;

static audio_device_t *default_hs = NULL;

static GSList *devices = NULL;

static void get_next_record(struct audio_sdp_data *data);
static DBusHandlerResult get_handles(const char *uuid,
					struct audio_sdp_data *data);

/* FIXME: Remove these once global error functions exist */
static DBusHandlerResult error_reply(DBusConnection *conn, DBusMessage *msg,
					const char *name, const char *descr)
{
	DBusMessage *derr;

	if (!conn || !msg)
		return DBUS_HANDLER_RESULT_HANDLED;

	derr = dbus_message_new_error(msg, name, descr);
	if (!derr) {
		error("Unable to allocate new error return");
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}

	return send_message_and_unref(conn, derr);
}

DBusHandlerResult err_invalid_args(DBusConnection *conn, DBusMessage *msg,
						const char *descr)
{
	return error_reply(conn, msg, "org.bluez.audio.Error.InvalidArguments",
			descr ? descr : "Invalid arguments in method call");
}

DBusHandlerResult err_already_connected(DBusConnection *conn, DBusMessage *msg)
{
	return error_reply(conn, msg, "org.bluez.audio.Error.AlreadyConnected",
				"Already connected to a device");
}

DBusHandlerResult err_not_connected(DBusConnection *conn, DBusMessage *msg)
{
	return error_reply(conn, msg, "org.bluez.audio.Error.NotConnected",
				"Not connected to any device");
}

DBusHandlerResult err_not_supported(DBusConnection *conn, DBusMessage *msg)
{
	return error_reply(conn, msg, "org.bluez.audio.Error.NotSupported",
			"The service is not supported by the remote device");
}

DBusHandlerResult err_connect_failed(DBusConnection *conn,
					DBusMessage *msg, int err)
{
	return error_reply(conn, msg, "org.bluez.audio.Error.ConnectFailed",
				strerror(err));
}

DBusHandlerResult err_does_not_exist(DBusConnection *conn, DBusMessage *msg)
{
	return error_reply(conn, msg, "org.bluez.audio.Error.DoesNotExist",
				"Does not exist");
}

DBusHandlerResult err_failed(DBusConnection *conn, DBusMessage *msg,
				const char *dsc)
{
	return error_reply(conn, msg, "org.bluez.audio.Error.Failed", dsc);
}

static audio_device_t *find_device(bdaddr_t *bda)
{
	GSList *l;

	for (l = devices; l != NULL; l = l->next) {
		audio_device_t *device = l->data;
		if (bacmp(&device->bda, bda) == 0)
			return device;
	}

	return NULL;
}

static DBusHandlerResult device_get_address(DBusConnection *conn, DBusMessage *msg,
						void *data)
{
	audio_device_t *device = data;
	DBusMessage *reply;
	char address[18], *ptr = address;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	ba2str(&device->bda, address);

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &ptr,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult device_get_connected(DBusConnection *conn,
						DBusMessage *msg,
						void *data)
{
	DBusMessageIter iter, array_iter;
	audio_device_t *device = data;
	DBusMessage *reply;
	const char *iface;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_STRING_AS_STRING, &array_iter);

	if (device->headset && headset_is_connected(device->headset)) {
		iface = AUDIO_HEADSET_INTERFACE;
		dbus_message_iter_append_basic(&array_iter,
						DBUS_TYPE_STRING, &iface);
	}

	dbus_message_iter_close_container(&iter, &array_iter);

	return send_message_and_unref(conn, reply);
}

static DBusMethodVTable device_methods[] = {
	{ "GetAddress",			device_get_address,
		"",	"s"	},
	{ "GetConnectedInterfaces",	device_get_connected,
		"",	"s"	},
	{ NULL, NULL, NULL, NULL }
};

static void free_device(audio_device_t *device)
{
	g_free(device);
}

static audio_device_t *create_device(bdaddr_t *bda)
{
	static int device_id = 0;
	audio_device_t *device;

	device = g_new0(audio_device_t, 1);

	bacpy(&device->bda, bda);

	snprintf(device->object_path, sizeof(device->object_path) - 1,
			"%s/device%d", AUDIO_MANAGER_PATH, device_id++);

	return device;
}

static void remove_device(audio_device_t *device)
{
	devices = g_slist_remove(devices, device);
	if (device->headset)
		headset_free(device->object_path);
	dbus_connection_destroy_object_path(connection, device->object_path);
	g_free(device);
}


static gboolean add_device(audio_device_t *device)
{
	if (!dbus_connection_create_object_path(connection, device->object_path,
						device, NULL)) {
		error("D-Bus failed to register %s path", device->object_path);
		return FALSE;
	}

	if (!dbus_connection_register_interface(connection, device->object_path,
						AUDIO_DEVICE_INTERFACE,
						device_methods, NULL, NULL)) {
		error("Failed to register %s interface to %s",
				AUDIO_DEVICE_INTERFACE, device->object_path);
		dbus_connection_destroy_object_path(connection,
							device->object_path);
		return FALSE;
	}

	devices = g_slist_append(devices, device);

	return TRUE;
}

static uint16_t get_service_uuid(const sdp_record_t *record)
{
	sdp_list_t *classes;
	uuid_t uuid;
	uint16_t uuid16 = 0;

	if (sdp_get_service_classes(record, &classes) < 0) {
		error("Unable to get service classes from record");
		return 0;
	}

	memcpy(&uuid, classes->data, sizeof(uuid));
	
	if (!sdp_uuid128_to_uuid(&uuid)) {
		error("Not a 16 bit UUID");
		sdp_list_free(classes, free);
		return 0;
	}

	if (uuid.type == SDP_UUID32) {
	       	if (uuid.value.uuid32 > 0xFFFF) {
			error("Not a 16 bit UUID");
			goto done;
		}
		uuid16 = (uint16_t) uuid.value.uuid32;
	} else
		uuid16 = uuid.value.uuid16;

done:
	sdp_list_free(classes, free);

	return uuid16;
}

void finish_sdp_transaction(DBusConnection *conn, bdaddr_t *dba) 
{
	char address[18], *addr_ptr = address;
	DBusMessage *msg, *reply;
	DBusError derr;

	ba2str(dba, address);

	msg = dbus_message_new_method_call("org.bluez", "/org/bluez/hci0",
						"org.bluez.Adapter",
						"FinishRemoteServiceTransaction");
	if (!msg) {
		error("Unable to allocate new method call");
		return;
	}

	dbus_message_append_args(msg, DBUS_TYPE_STRING, &addr_ptr,
					DBUS_TYPE_INVALID);

	dbus_error_init(&derr);
	reply = dbus_connection_send_with_reply_and_block(conn, msg, -1,
								&derr);

	dbus_message_unref(msg);

	if (dbus_error_is_set(&derr) || dbus_set_error_from_message(&derr, reply)) {
		error("FinishRemoteServiceTransaction(%s) failed: %s",
				address, derr.message);
		dbus_error_free(&derr);
		return;
	}

	dbus_message_unref(reply);
}

static void handle_record(sdp_record_t *record, audio_device_t *device)
{
	uint16_t uuid16;

	uuid16 = get_service_uuid(record);

	switch (uuid16) {
	case HEADSET_SVCLASS_ID:
		debug("Found Headset record");
		if (device->headset)
			headset_update(device->headset, record, uuid16);
		else
			device->headset = headset_init(device->object_path,
							record, uuid16);
		break;
	case HEADSET_AGW_SVCLASS_ID:
		debug("Found Headset AG record");
		break;
	case HANDSFREE_SVCLASS_ID:
		debug("Found Hansfree record");
		break;
	case HANDSFREE_AGW_SVCLASS_ID:
		debug("Found Handsfree AG record");
		break;
	case AUDIO_SINK_SVCLASS_ID:
		debug("Found Audio Sink");
		break;
	case AUDIO_SOURCE_SVCLASS_ID:
		debug("Found Audio Source");
		break;
	case AV_REMOTE_SVCLASS_ID:
		debug("Found AV Remote");
		break;
	case AV_REMOTE_TARGET_SVCLASS_ID:
		debug("Found AV Target");
		break;
	default:
		debug("Unrecognized UUID: 0x%04X", uuid16);
		break;
	}
}

static gint record_iface_cmp(gconstpointer a, gconstpointer b)
{
	const sdp_record_t *record = a;
	const char *interface = b;

	switch (get_service_uuid(record)) {
	case HEADSET_SVCLASS_ID:
	case HANDSFREE_SVCLASS_ID:
		return strcmp(interface, AUDIO_HEADSET_INTERFACE);

	case HEADSET_AGW_SVCLASS_ID:
	case HANDSFREE_AGW_SVCLASS_ID:
		return strcmp(interface, AUDIO_GATEWAY_INTERFACE);

	case AUDIO_SINK_SVCLASS_ID:
		return strcmp(interface, AUDIO_SINK_INTERFACE);

	case AUDIO_SOURCE_SVCLASS_ID:
		return strcmp(interface, AUDIO_SOURCE_INTERFACE);

	case AV_REMOTE_SVCLASS_ID:
		return strcmp(interface, AUDIO_CONTROL_INTERFACE);

	case AV_REMOTE_TARGET_SVCLASS_ID:
		return strcmp(interface, AUDIO_TARGET_INTERFACE);

	default:
		return -1;
	}
}

static void finish_sdp(struct audio_sdp_data *data, gboolean success)
{
	const char *path, *addr;
	char **required;
	int required_len, i;
	DBusMessage *reply = NULL;

	debug("Audio service discovery completed with %s",
			success ? "success" : "failure");

	finish_sdp_transaction(connection, &data->device->bda);

	if (!success)
		goto done;

	if (!data->msg)
		goto update;

	if (!dbus_message_get_args(data->msg, NULL,
					DBUS_TYPE_STRING, &addr,
					DBUS_TYPE_ARRAY, DBUS_TYPE_STRING,
					&required, &required_len,
					DBUS_TYPE_INVALID)) {
		error("Unable to get message args");
		success = FALSE;
		goto done;
	}

	/* Return error if no audio related service records were found */
	if (!data->records) {
		debug("No audio audio related service records were found");
		success = FALSE;
		err_not_supported(connection, data->msg);
		goto done;
	}

	for (i = 0; i < required_len; i++) {
		const char *iface = required[i];

		if (g_slist_find_custom(data->records, iface, record_iface_cmp))
			continue;

		debug("Required interface %s not supported", iface);
		success = FALSE;
		err_not_supported(connection, data->msg);
		dbus_free_string_array(required);
		goto done;
	}

	dbus_free_string_array(required);

	path = data->device->object_path;

	reply = dbus_message_new_method_return(data->msg);
	if (!reply) {
		success = FALSE;
		err_failed(connection, data->msg, "Out of memory");
		goto done;
	}

	add_device(data->device);

update:
	g_slist_foreach(data->records, (GFunc) handle_record, data->device);

	if (reply) {
		dbus_connection_emit_signal(connection, AUDIO_MANAGER_PATH,
						AUDIO_MANAGER_INTERFACE,
						"DeviceCreated",
						DBUS_TYPE_STRING, &path,
						DBUS_TYPE_INVALID);

		dbus_message_append_args(reply, DBUS_TYPE_STRING, &path,
				DBUS_TYPE_INVALID);
		send_message_and_unref(connection, reply);
	}

done:
	if (!success)
		free_device(data->device);
	if (data->msg)
		dbus_message_unref(data->msg);
	g_slist_foreach(data->handles, (GFunc) g_free, NULL);
	g_slist_free(data->handles);
	g_slist_foreach(data->records, (GFunc) sdp_record_free, NULL);
	g_slist_free(data->records);
	g_free(data);
}

static void get_record_reply(DBusPendingCall *call,
				struct audio_sdp_data *data)
{
	DBusMessage *reply;
	DBusError derr;
	uint8_t *array;
	int array_len, record_len;
	sdp_record_t *record;

	reply = dbus_pending_call_steal_reply(call);

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		error("GetRemoteServiceRecord failed: %s", derr.message);
		if (dbus_error_has_name(&derr,
					"org.bluez.Error.ConnectionAttemptFailed"))
			err_connect_failed(connection, data->msg, EHOSTDOWN);
		else
			err_failed(connection, data->msg, derr.message);
		dbus_error_free(&derr);
		goto failed;
	}

	if (!dbus_message_get_args(reply, NULL,
				DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &array, &array_len,
				DBUS_TYPE_INVALID)) {
		err_failed(connection, data->msg,
				"Unable to get args from GetRecordReply");
		goto failed;
	}

	record = sdp_extract_pdu(array, &record_len);
	if (!record) {
		error("Unable to extract service record from reply");
		goto done;
	}

	if (record_len != array_len)
		debug("warning: array len (%d) != record len (%d)",
				array_len, record_len);

	data->records = g_slist_append(data->records, record);

done:
	dbus_message_unref(reply);

	if (data->handles)
		get_next_record(data);
	else
		finish_sdp(data, TRUE);

	return;

failed:
	if (reply)
		dbus_message_unref(reply);
	finish_sdp(data, FALSE);
}

static void get_next_record(struct audio_sdp_data *data)
{
	DBusMessage *msg;
	DBusPendingCall *pending;
	char address[18], *ptr = address;
	dbus_uint32_t *handle;

	msg = dbus_message_new_method_call("org.bluez", "/org/bluez/hci0",
						"org.bluez.Adapter",
						"GetRemoteServiceRecord");
	if (!msg) {
		error("Unable to allocate new method call");
		err_connect_failed(connection, data->msg, ENOMEM);
		finish_sdp(data, FALSE);
		return;
	}

	handle = data->handles->data;

	data->handles = g_slist_remove(data->handles, data->handles->data);

	ba2str(&data->device->bda, address);

	dbus_message_append_args(msg, DBUS_TYPE_STRING, &ptr,
					DBUS_TYPE_UINT32, handle,
					DBUS_TYPE_INVALID);

	g_free(handle);

	if (!dbus_connection_send_with_reply(connection, msg, &pending, -1)) {
		error("Sending GetRemoteServiceRecord failed");
		err_connect_failed(connection, data->msg, EIO);
		finish_sdp(data, FALSE);
		return;
	}

	dbus_pending_call_set_notify(pending,
			(DBusPendingCallNotifyFunction) get_record_reply,
			data, NULL);
	dbus_pending_call_unref(pending);
	dbus_message_unref(msg);
}

static GSList *find_handle(GSList *handles, dbus_uint32_t handle)
{
	while (handles) {
		if (*(dbus_uint32_t *) handles->data == handle)
			return handles;
		handles = handles->next;
	}

	return NULL;
}

static void get_handles_reply(DBusPendingCall *call,
				struct audio_sdp_data *data)
{
	DBusMessage *reply;
	DBusError derr;
	dbus_uint32_t *array = NULL;
	int array_len, i;

	reply = dbus_pending_call_steal_reply(call);

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		error("GetRemoteServiceHandles failed: %s", derr.message);
		if (dbus_error_has_name(&derr,
					"org.bluez.Error.ConnectionAttemptFailed"))
			err_connect_failed(connection, data->msg, EHOSTDOWN);
		else
			err_failed(connection, data->msg, derr.message);
		dbus_error_free(&derr);
		goto failed;
	}

	if (!dbus_message_get_args(reply, NULL,
				DBUS_TYPE_ARRAY, DBUS_TYPE_UINT32, &array, &array_len,
				DBUS_TYPE_INVALID)) {
	  
		err_failed(connection, data->msg,
				"Unable to get args from reply");
		goto failed;
	}

	for (i = 0; i < array_len; i++) {
		if (!find_handle(data->handles, array[i])) {
			dbus_uint32_t *handle = g_new(dbus_uint32_t, 1);
			*handle = array[i];
			data->handles = g_slist_append(data->handles, handle);
		}
	}

	data->state++;

	switch (data->state) {
	case ADVANCED_AUDIO:
		get_handles(ADVANCED_AUDIO_UUID, data);
		break;
	case AV_REMOTE:
		get_handles(AVRCP_REMOTE_UUID, data);
		break;
	default:
		if (data->handles)
			get_next_record(data);
		else
			finish_sdp(data, TRUE);
	}	

	dbus_message_unref(reply);

	return;

failed:
	dbus_message_unref(reply);
	finish_sdp(data, FALSE);
}

static DBusHandlerResult get_handles(const char *uuid,
					struct audio_sdp_data *data)
{
	DBusPendingCall *pending;
	char address[18];
	const char *ptr = address;
	DBusMessage *msg;

	msg = dbus_message_new_method_call("org.bluez", "/org/bluez/hci0",
						"org.bluez.Adapter",
						"GetRemoteServiceHandles");
	if (!msg) {
		err_failed(connection, data->msg,
				"Could not create a new dbus message");
		goto failed;
	}

	ba2str(&data->device->bda, address);

	dbus_message_append_args(msg, DBUS_TYPE_STRING, &ptr,
					DBUS_TYPE_STRING, &uuid,
					DBUS_TYPE_INVALID);

	if (!dbus_connection_send_with_reply(connection, msg, &pending, -1)) {
		err_failed(connection, data->msg,
				"Sending GetRemoteServiceHandles failed");
		goto failed;
	}

	dbus_pending_call_set_notify(pending,
			(DBusPendingCallNotifyFunction) get_handles_reply,
			data, NULL);
	dbus_pending_call_unref(pending);
	dbus_message_unref(msg);

	return DBUS_HANDLER_RESULT_HANDLED;

failed:
	if (msg)
		dbus_message_unref(msg);
	finish_sdp(data, FALSE);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult resolve_services(DBusMessage *msg,
						audio_device_t *device)
{
	struct audio_sdp_data *sdp_data;

	sdp_data = g_new0(struct audio_sdp_data, 1);
	if (msg)
		sdp_data->msg = dbus_message_ref(msg);
	sdp_data->device = device;

	return get_handles(GENERIC_AUDIO_UUID, sdp_data);
}

audio_device_t *manager_headset_connected(bdaddr_t *bda)
{
	audio_device_t *device;
	const char *path;
	gboolean created = FALSE;

	device = find_device(bda);
	if (device && device->headset)
		return device;

	if (!device) {
		device = create_device(bda);
		if (!add_device(device)) {
			free_device(device);
			return NULL;
		}
		created = TRUE;
	}

	if (!device->headset)
		device->headset = headset_init(device->object_path, NULL, 0);

	if (!device->headset)
		return NULL;

	path = device->object_path;

	if (created) {
		dbus_connection_emit_signal(connection, AUDIO_MANAGER_PATH,
						AUDIO_MANAGER_INTERFACE,
						"DeviceCreated",
						DBUS_TYPE_STRING, &path,
						DBUS_TYPE_INVALID);
		resolve_services(NULL, device);
	}

	dbus_connection_emit_signal(connection, AUDIO_MANAGER_PATH,
					AUDIO_MANAGER_INTERFACE,
					"HeadsetCreated",
					DBUS_TYPE_STRING, &path,
					DBUS_TYPE_INVALID);

	if (!default_hs) {
		default_hs = device;
		dbus_connection_emit_signal(connection, AUDIO_MANAGER_PATH,
						AUDIO_MANAGER_INTERFACE,
						"DefaultHeadsetChanged",
						DBUS_TYPE_STRING, &path,
						DBUS_TYPE_INVALID);
	}

	return device;
}

static gboolean device_supports_interface(audio_device_t *device,
						const char *iface)
{
		if (strcmp(iface, AUDIO_HEADSET_INTERFACE) == 0)
		       	return device->headset ? TRUE : FALSE;

		if (strcmp(iface, AUDIO_GATEWAY_INTERFACE) == 0)
			return device->gateway ? TRUE : FALSE;

		if (strcmp(iface, AUDIO_SOURCE_INTERFACE) == 0)
			return device->gateway ? TRUE : FALSE;

		if (strcmp(iface, AUDIO_SINK_INTERFACE) == 0)
			return device->sink ? TRUE : FALSE;

		if (strcmp(iface, AUDIO_CONTROL_INTERFACE) == 0)
			return device->control ? TRUE : FALSE;

		if (strcmp(iface, AUDIO_TARGET_INTERFACE) == 0)
			return device->target ? TRUE : FALSE;

		debug("Unknown interface %s", iface);

		return FALSE;
}

static gboolean device_matches(audio_device_t *device, char **interfaces)
{
	int i;

	for (i = 0; interfaces[i]; i++) {
		if (device_supports_interface(device, interfaces[i]))
			continue;
		debug("Device does not support interface %s", interfaces[i]);
		return FALSE;
	}

	return TRUE;
}

static DBusHandlerResult am_create_device(DBusConnection *conn, DBusMessage *msg,
						void *data)
{
	const char *address, *path;
	char **required;
	int required_len;
	bdaddr_t bda;
	audio_device_t *device;
	DBusMessage *reply;
	DBusError derr;

	dbus_error_init(&derr);
	dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &address,
				DBUS_TYPE_ARRAY, DBUS_TYPE_STRING,
				&required, &required_len,
				DBUS_TYPE_INVALID);
	if (dbus_error_is_set(&derr)) {
		err_invalid_args(connection, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	str2ba(address, &bda);

	device = find_device(&bda);
	if (!device) {
		device = create_device(&bda);
		dbus_free_string_array(required);
		return resolve_services(msg, device);
	}

	if (!device_matches(device, required)) {
		dbus_free_string_array(required);
		return err_not_supported(conn, msg);
	}

	dbus_free_string_array(required);

	path = device->object_path;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &path,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult am_list_devices(DBusConnection *conn,
						DBusMessage *msg,
						void *data)
{
	DBusMessageIter iter, array_iter;
	DBusMessage *reply;
	DBusError derr;
	GSList *l;
	char **required;
	int required_len;

	dbus_error_init(&derr);
	dbus_message_get_args(msg, &derr,
				DBUS_TYPE_ARRAY, DBUS_TYPE_STRING,
				&required, &required_len,
				DBUS_TYPE_INVALID);
	if (dbus_error_is_set(&derr)) {
		err_invalid_args(connection, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_STRING_AS_STRING, &array_iter);

	for (l = devices; l != NULL; l = l->next) {
		audio_device_t *device = l->data;
		const char *path;

		if (!device_matches(device, required))
			continue;

		path = device->object_path;

		dbus_message_iter_append_basic(&array_iter,
						DBUS_TYPE_STRING, &path);
	}

	dbus_message_iter_close_container(&iter, &array_iter);

	dbus_free_string_array(required);

	return send_message_and_unref(connection, reply);
}

static DBusHandlerResult am_create_headset(DBusConnection *conn, DBusMessage *msg,
						void *data)
{
	const char *path, *address;
	bdaddr_t bda;
	DBusMessage *reply;
	DBusError derr;
	audio_device_t *device;

	dbus_error_init(&derr);
	dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &address,
				DBUS_TYPE_INVALID);
	if (dbus_error_is_set(&derr)) {
		err_invalid_args(connection, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	str2ba(address, &bda);

	device = find_device(&bda);
	if (!device) {
		device = create_device(&bda);
		if (!add_device(device)) {
			free_device(device);
			return error_reply(connection, msg,
					"org.bluez.audio.Error.Failed",
					"Unable to create new audio device");
		}
	}

	if (!device->headset) {
		device->headset = headset_init(device->object_path, NULL, 0);
		if (!device->headset) {
			remove_device(device);
			return error_reply(connection, msg,
						"org.bluez.audio.Error.Failed",
						"Unable to init Headset interface");
		}
	}

	path = device->object_path;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_connection_emit_signal(connection, AUDIO_MANAGER_PATH,
					AUDIO_MANAGER_INTERFACE,
					"HeadsetCreated",
					DBUS_TYPE_STRING, &path,
					DBUS_TYPE_INVALID);

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &path,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(connection, reply);
}

static gint device_path_cmp(gconstpointer a, gconstpointer b)
{
	const audio_device_t *device = a;
	const char *path = b;

	return strcmp(device->object_path, path);
}

static DBusHandlerResult am_remove_device(DBusConnection *conn,
						DBusMessage *msg,
						void *data)
{
	DBusError derr;
	DBusMessage *reply;
	GSList *match;
	const char *path;
	audio_device_t *device;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
					DBUS_TYPE_STRING, &path,
					DBUS_TYPE_INVALID)) {
		err_invalid_args(connection, msg, derr.message);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (dbus_error_is_set(&derr)) {
		err_invalid_args(connection, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	match = g_slist_find_custom(devices, path, device_path_cmp);
	if (!match)
		return error_reply(connection, msg,
					"org.bluez.audio.Error.DoesNotExist",
					"The headset does not exist");

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	device = match->data;

	remove_device(device);

	if (default_hs == device) {
		const char *param;
		GSList *l;

		default_hs = NULL;

		for (l = devices; l != NULL; l = l->next) {
			device = l->data;

			if (device->headset)
				default_hs = device;
		}

		param = default_hs ? default_hs->object_path : "";

		dbus_connection_emit_signal(conn, AUDIO_MANAGER_PATH,
						AUDIO_MANAGER_INTERFACE,
						"DefaultHeadsetChanged",
						DBUS_TYPE_STRING, &param,
						DBUS_TYPE_INVALID);
	}

	dbus_connection_emit_signal(conn, AUDIO_MANAGER_PATH,
					AUDIO_MANAGER_INTERFACE,
					"HeadsetRemoved",
					DBUS_TYPE_STRING, &path,
					DBUS_TYPE_INVALID);

	dbus_connection_emit_signal(conn, AUDIO_MANAGER_PATH,
					AUDIO_MANAGER_INTERFACE,
					"DeviceRemoved",
					DBUS_TYPE_STRING, &path,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(connection, reply);
}

static DBusHandlerResult am_remove_headset(DBusConnection *conn,
						DBusMessage *msg,
						void *data)
{
	return am_remove_device(conn, msg, data);
}

static DBusHandlerResult am_list_headsets(DBusConnection *conn, DBusMessage *msg,
						void *data)
{
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	DBusMessage *reply;
	GSList *l;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_STRING_AS_STRING, &array_iter);

	for (l = devices; l != NULL; l = l->next) {
		audio_device_t *device = l->data;
		const char *path;

		if (!device->headset)
			continue;

		path = device->object_path;

		dbus_message_iter_append_basic(&array_iter,
						DBUS_TYPE_STRING, &path);
	}

	dbus_message_iter_close_container(&iter, &array_iter);

	return send_message_and_unref(connection, reply);
}

static DBusHandlerResult am_find_by_addr(DBusConnection *conn, DBusMessage *msg,
						void *data)
{
	const char *path, *address;
	DBusMessage *reply;
	DBusError derr;
	audio_device_t *device;
	bdaddr_t bda;

	dbus_error_init(&derr);
	dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &address,
				DBUS_TYPE_INVALID);
	if (dbus_error_is_set(&derr)) {
		err_invalid_args(connection, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	str2ba(address, &bda);

	device = find_device(&bda);

	if (!device)
		return err_does_not_exist(conn, msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	path = device->object_path;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &path,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult am_get_default_headset(DBusConnection *conn, DBusMessage *msg,
						void *data)
{
	DBusMessage *reply;
	const char *path;

	if (!default_hs)
		return error_reply(connection, msg,
					"org.bluez.audio.Error.DoesNotExist",
					"There is no default headset");

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	path = default_hs->object_path;

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &path,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(connection, reply);
}

static DBusHandlerResult am_change_default_headset(DBusConnection *conn, DBusMessage *msg,
							void *data)
{
	DBusError derr;
	DBusMessage *reply;
	GSList *match;
	const char *path;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
					DBUS_TYPE_STRING, &path,
					DBUS_TYPE_INVALID)) {
		err_invalid_args(connection, msg, derr.message);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (dbus_error_is_set(&derr)) {
		err_invalid_args(connection, msg, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	match = g_slist_find_custom(devices, path, device_path_cmp);
	if (!match)
		return error_reply(connection, msg,
					"org.bluez.audio.Error.DoesNotExist",
					"The headset does not exist");

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	default_hs = match->data;

	path = default_hs->object_path;

	dbus_connection_emit_signal(conn, AUDIO_MANAGER_PATH,
					AUDIO_MANAGER_INTERFACE,
					"DefaultHeadsetChanged",
					DBUS_TYPE_STRING, &path,
					DBUS_TYPE_INVALID);

	return send_message_and_unref(connection, reply);
}

static DBusMethodVTable manager_methods[] = {
	{ "CreateDevice",		am_create_device,
		"sas",	"s"		},
	{ "RemoveDevice",		am_remove_device,
		"s",	""		},
	{ "ListDevices",		am_list_devices,
		"as",	"as"	},
	{ "CreateHeadset",		am_create_headset,
		"s",	"s"		},
	{ "RemoveHeadset",		am_remove_headset,
		"s",	""		},
	{ "ListHeadsets",		am_list_headsets,
		"",	"as"		},
	{ "FindDeviceByAddress",	am_find_by_addr,
		"s",	"s"		},
	{ "DefaultHeadset",		am_get_default_headset,
		"",	"s"		},
	{ "ChangeDefaultHeadset",	am_change_default_headset,
		"s",	""	},
	{ NULL, NULL, NULL, NULL },
};

static DBusSignalVTable manager_signals[] = {
	{ "DeviceCreated",		"s"	},
	{ "DeviceRemoved",		"s"	},
	{ "HeadsetCreated",		"s"	},
	{ "HeadsetRemoved",		"s"	},
	{ "DefaultHeadsetChanged",	"s"	},
	{ NULL, NULL }
};

int audio_init(DBusConnection *conn)
{
	if (!dbus_connection_create_object_path(conn, AUDIO_MANAGER_PATH,
						NULL, NULL)) {
		error("D-Bus failed to register %s path", AUDIO_MANAGER_PATH);
		return -1;
	}

	if (!dbus_connection_register_interface(conn, AUDIO_MANAGER_PATH,
						AUDIO_MANAGER_INTERFACE,
						manager_methods,
						manager_signals, NULL)) {
		error("Failed to register %s interface to %s",
				AUDIO_MANAGER_INTERFACE, AUDIO_MANAGER_PATH);
		dbus_connection_destroy_object_path(conn,
							AUDIO_MANAGER_PATH);
		return -1;
	}

	connection = dbus_connection_ref(conn);

	return 0;
}

void audio_exit(void)
{
	g_slist_foreach(devices, (GFunc) remove_device, NULL);
	g_slist_free(devices);
	devices = NULL;

	dbus_connection_unref(connection);

	connection = NULL;
}

int manager_get_device(uint8_t role, struct ipc_data_cfg *cfg)
{
	if (default_hs == NULL || default_hs->headset == NULL)
		return -1;

	if (!headset_is_connected(default_hs->headset))
		return -1;

	return headset_get_config(default_hs->headset, cfg);
}
