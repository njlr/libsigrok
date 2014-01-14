/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2014 Daniel Elstner <daniel.kitta@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "protocol.h"
#include "libsigrok.h"
#include "libsigrok-internal.h"
#include <glib.h>
#include <libusb.h>
#include <stdlib.h>
#include <string.h>

static const int32_t hwcaps[] = {
	SR_CONF_LOGIC_ANALYZER,
	SR_CONF_SAMPLERATE,
	SR_CONF_EXTERNAL_CLOCK,
	SR_CONF_TRIGGER_TYPE,
	SR_CONF_LIMIT_SAMPLES,
};

/* The hardware supports more samplerates than these, but these are the
 * options hardcoded into the vendor's Windows GUI.
 */
static const uint64_t samplerates[] = {
	SR_MHZ(125), SR_MHZ(100),
	SR_MHZ(50),  SR_MHZ(20),  SR_MHZ(10),
	SR_MHZ(5),   SR_MHZ(2),   SR_MHZ(1),
	SR_KHZ(500), SR_KHZ(200), SR_KHZ(100),
	SR_KHZ(50),  SR_KHZ(20),  SR_KHZ(10),
	SR_KHZ(5),   SR_KHZ(2),   SR_KHZ(1),
	SR_HZ(500),  SR_HZ(200),  SR_HZ(100),
};

SR_PRIV struct sr_dev_driver sysclk_lwla_driver_info;
static struct sr_dev_driver *const di = &sysclk_lwla_driver_info;

static int init(struct sr_context *sr_ctx)
{
	return std_init(sr_ctx, di, LOG_PREFIX);
}

static GSList *gen_probe_list(int num_probes)
{
	GSList *list;
	struct sr_probe *probe;
	int i;
	char name[8];

	list = NULL;

	for (i = num_probes; i > 0; --i) {
		/* The LWLA series simply number probes from CH1 to CHxx. */
		g_ascii_formatd(name, sizeof name, "CH%.0f", i);

		probe = sr_probe_new(i - 1, SR_PROBE_LOGIC, TRUE, name);
		list = g_slist_prepend(list, probe);
	}

	return list;
}

static GSList *scan(GSList *options)
{
	GSList *usb_devices, *devices, *node;
	struct drv_context *drvc;
	struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct sr_usb_dev_inst *usb;
	int device_index;

	(void)options;

	devices = NULL;
	drvc = di->priv;
	drvc->instances = NULL;
	device_index = 0;

	usb_devices = sr_usb_find(drvc->sr_ctx->libusb_ctx, USB_VID_PID);

	for (node = usb_devices; node != NULL; node = node->next) {
		usb = node->data;

		/* Allocate memory for our private driver context. */
		devc = g_try_new0(struct dev_context, 1);
		if (!devc) {
			sr_err("Device context malloc failed.");
			sr_usb_dev_inst_free(usb);
			continue;
		}
		/* Register the device with libsigrok. */
		sdi = sr_dev_inst_new(device_index, SR_ST_INACTIVE,
				      VENDOR_NAME, MODEL_NAME, NULL);
		if (!sdi) {
			sr_err("Failed to instantiate device.");
			g_free(devc);
			sr_usb_dev_inst_free(usb);
			continue;
		}
		sdi->driver = di;
		sdi->priv = devc;
		sdi->inst_type = SR_INST_USB;
		sdi->conn = usb;
		sdi->probes = gen_probe_list(NUM_PROBES);

		drvc->instances = g_slist_append(drvc->instances, sdi);
		devices = g_slist_append(devices, sdi);
	}

	g_slist_free(usb_devices);

	return devices;
}

static GSList *dev_list(void)
{
	struct drv_context *drvc;

	drvc = di->priv;

	return drvc->instances;
}

static void clear_dev_context(void *priv)
{
	struct dev_context *devc;

	devc = priv;

	sr_dbg("Device context cleared.");

	lwla_free_acquisition_state(devc->acquisition);
	g_free(devc);
}

static int dev_clear(void)
{
	return std_dev_clear(di, &clear_dev_context);
}

static int dev_open(struct sr_dev_inst *sdi)
{
	struct drv_context *drvc;
	struct dev_context *devc;
	struct sr_usb_dev_inst *usb;
	int ret;

	drvc = di->priv;

	if (!drvc) {
		sr_err("Driver was not initialized.");
		return SR_ERR;
	}

	usb  = sdi->conn;
	devc = sdi->priv;

	ret = sr_usb_open(drvc->sr_ctx->libusb_ctx, usb);
	if (ret != SR_OK)
		return ret;

	ret = libusb_claim_interface(usb->devhdl, USB_INTERFACE);
	if (ret < 0) {
		sr_err("Failed to claim interface: %s.",
			libusb_error_name(ret));
		return SR_ERR;
	}

	sdi->status = SR_ST_INITIALIZING;

	if (devc->samplerate == 0)
		/* Apply default if the samplerate hasn't been set yet. */
		devc->samplerate = DEFAULT_SAMPLERATE;

	ret = lwla_init_device(sdi);

	if (ret == SR_OK)
		sdi->status = SR_ST_ACTIVE;

	return ret;
}

static int dev_close(struct sr_dev_inst *sdi)
{
	struct sr_usb_dev_inst *usb;
	struct dev_context *devc;

	if (!di->priv) {
		sr_err("Driver was not initialized.");
		return SR_ERR;
	}

	usb  = sdi->conn;
	devc = sdi->priv;

	if (!usb->devhdl)
		return SR_OK;

	/* Trigger download of the shutdown bitstream. */
	devc->selected_clock_source = CLOCK_SOURCE_NONE;

	if (lwla_set_clock_source(sdi) != SR_OK)
		sr_err("Unable to shut down device.");

	libusb_release_interface(usb->devhdl, USB_INTERFACE);
	libusb_close(usb->devhdl);

	usb->devhdl = NULL;
	sdi->status = SR_ST_INACTIVE;

	return SR_OK;
}

static int cleanup(void)
{
	return dev_clear();
}

static int config_get(int key, GVariant **data, const struct sr_dev_inst *sdi,
		      const struct sr_probe_group *probe_group)
{
	struct dev_context *devc;

	(void)probe_group;

	if (!sdi)
		return SR_ERR_ARG;

	devc = sdi->priv;

	switch (key) {
	case SR_CONF_SAMPLERATE:
		*data = g_variant_new_uint64(devc->samplerate);
		break;
	case SR_CONF_LIMIT_SAMPLES:
		*data = g_variant_new_uint64(devc->limit_samples);
		break;
	case SR_CONF_EXTERNAL_CLOCK:
		*data = g_variant_new_boolean(devc->selected_clock_source
						>= CLOCK_SOURCE_EXT_RISE);
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int config_set(int key, GVariant *data, const struct sr_dev_inst *sdi,
		      const struct sr_probe_group *probe_group)
{
	struct dev_context *devc;
	uint64_t rate;

	(void)probe_group;

	devc = sdi->priv;
	if (!devc)
		return SR_ERR_DEV_CLOSED;

	switch (key) {
	case SR_CONF_SAMPLERATE:
		rate = g_variant_get_uint64(data);
		sr_info("Setting samplerate %" G_GUINT64_FORMAT, rate);
		if (rate > samplerates[0]
		    || rate < samplerates[G_N_ELEMENTS(samplerates) - 1])
			return SR_ERR_SAMPLERATE;
		devc->samplerate = rate;
		break;
	case SR_CONF_LIMIT_SAMPLES:
		devc->limit_samples = g_variant_get_uint64(data);
		break;
	case SR_CONF_EXTERNAL_CLOCK:
		if (g_variant_get_boolean(data)) {
			sr_info("Enabling external clock.");
			/* TODO: Allow the external clock to be inverted */
			devc->selected_clock_source = CLOCK_SOURCE_EXT_RISE;
		} else {
			sr_info("Disabling external clock.");
			devc->selected_clock_source = CLOCK_SOURCE_INT;
		}
		if (sdi->status == SR_ST_ACTIVE)
			return lwla_set_clock_source(sdi);
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int config_list(int key, GVariant **data, const struct sr_dev_inst *sdi,
		       const struct sr_probe_group *probe_group)
{
	GVariant *gvar;
	GVariantBuilder gvb;

	(void)sdi;
	(void)probe_group;

	switch (key) {
	case SR_CONF_DEVICE_OPTIONS:
		*data = g_variant_new_fixed_array(G_VARIANT_TYPE_INT32,
				hwcaps, ARRAY_SIZE(hwcaps), sizeof(int32_t));
		break;
	case SR_CONF_SAMPLERATE:
		g_variant_builder_init(&gvb, G_VARIANT_TYPE("a{sv}"));
		gvar = g_variant_new_fixed_array(G_VARIANT_TYPE("t"),
				samplerates, ARRAY_SIZE(samplerates),
				sizeof(uint64_t));
		g_variant_builder_add(&gvb, "{sv}", "samplerates", gvar);
		*data = g_variant_builder_end(&gvb);
		break;
	case SR_CONF_TRIGGER_TYPE:
		*data = g_variant_new_string(TRIGGER_TYPES);
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int configure_probes(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	const struct sr_probe *probe;
	const GSList *node;
	uint64_t probe_bit;

	devc = sdi->priv;

	devc->channel_mask = 0;
	devc->trigger_mask = 0;
	devc->trigger_edge_mask = 0;
	devc->trigger_values = 0;

	for (node = sdi->probes, probe_bit = 1;
			node != NULL;
			node = node->next, probe_bit <<= 1) {

		if (probe_bit >= ((uint64_t)1 << NUM_PROBES)) {
			sr_err("Channels over the limit of %d.", NUM_PROBES);
			return SR_ERR;
		}
		probe = node->data;
		if (!probe || !probe->enabled)
			continue;

		/* Enable input channel for this probe. */
		devc->channel_mask |= probe_bit;

		if (!probe->trigger || probe->trigger[0] == '\0')
			continue;

		if (probe->trigger[1] != '\0') {
			sr_err("Only one trigger stage is supported.");
			return SR_ERR;
		}
		/* Enable trigger for this probe. */
		devc->trigger_mask |= probe_bit;

		/* Configure edge mask and trigger value. */
		switch (probe->trigger[0]) {
		case '1': devc->trigger_values |= probe_bit;
		case '0': break;

		case 'r': devc->trigger_values |= probe_bit;
		case 'f': devc->trigger_edge_mask |= probe_bit;
			  break;
		default:
			sr_err("Trigger type '%c' is not supported.",
			       probe->trigger[0]);
			return SR_ERR;
		}
	}
	return SR_OK;
}

static int dev_acquisition_start(const struct sr_dev_inst *sdi, void *cb_data)
{
	struct drv_context *drvc;
	struct dev_context *devc;
	struct acquisition_state *acq;
	int ret;

	(void)cb_data;

	if (sdi->status != SR_ST_ACTIVE)
		return SR_ERR_DEV_CLOSED;

	devc = sdi->priv;
	drvc = di->priv;

	if (devc->acquisition) {
		sr_err("Acquisition still in progress?");
		return SR_ERR;
	}
	acq = lwla_alloc_acquisition_state();
	if (!acq)
		return SR_ERR_MALLOC;

	devc->stopping_in_progress = FALSE;
	devc->transfer_error = FALSE;

	ret = configure_probes(sdi);
	if (ret != SR_OK) {
		sr_err("Failed to configure probes.");
		lwla_free_acquisition_state(acq);
		return ret;
	}

	sr_info("Starting acquisition.");

	devc->acquisition = acq;
	ret = lwla_setup_acquisition(sdi);
	if (ret != SR_OK) {
		sr_err("Failed to set up aquisition.");
		devc->acquisition = NULL;
		lwla_free_acquisition_state(acq);
		return ret;
	}

	ret = lwla_start_acquisition(sdi);
	if (ret != SR_OK) {
		sr_err("Failed to start aquisition.");
		devc->acquisition = NULL;
		lwla_free_acquisition_state(acq);
		return ret;
	}
	usb_source_add(drvc->sr_ctx, 100, &lwla_receive_data,
		       (struct sr_dev_inst *)sdi);

	sr_info("Waiting for data.");

	/* Send header packet to the session bus. */
	std_session_send_df_header(sdi, LOG_PREFIX);

	return SR_OK;
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi, void *cb_data)
{
	(void)cb_data;

	if (sdi->status != SR_ST_ACTIVE)
		return SR_ERR_DEV_CLOSED;

	sr_dbg("Stopping acquisition.");

	sdi->status = SR_ST_STOPPING;

	return SR_OK;
}

SR_PRIV struct sr_dev_driver sysclk_lwla_driver_info = {
	.name = "sysclk-lwla",
	.longname = "SysClk LWLA series",
	.api_version = 1,
	.init = init,
	.cleanup = cleanup,
	.scan = scan,
	.dev_list = dev_list,
	.dev_clear = dev_clear,
	.config_get = config_get,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = dev_open,
	.dev_close = dev_close,
	.dev_acquisition_start = dev_acquisition_start,
	.dev_acquisition_stop = dev_acquisition_stop,
	.priv = NULL,
};