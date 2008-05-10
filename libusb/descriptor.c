/*
 * USB descriptor handling functions for libusb
 * Copyright (C) 2007 Daniel Drake <dsd@gentoo.org>
 * Copyright (c) 2001 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "libusbi.h"

#define DESC_HEADER_LENGTH		2
#define DEVICE_DESC_LENGTH		18
#define CONFIG_DESC_LENGTH		9
#define INTERFACE_DESC_LENGTH		9
#define ENDPOINT_DESC_LENGTH		7
#define ENDPOINT_AUDIO_DESC_LENGTH	9

/** @defgroup desc USB descriptors
 * This page details how to examine the various standard USB descriptors
 * for detected devices
 */

int usbi_parse_descriptor(unsigned char *source, char *descriptor, void *dest)
{
	unsigned char *sp = source, *dp = dest;
	uint16_t w;
	uint32_t d;
	char *cp;

	for (cp = descriptor; *cp; cp++) {
		switch (*cp) {
			case 'b':	/* 8-bit byte */
				*dp++ = *sp++;
				break;
			case 'w':	/* 16-bit word, convert from little endian to CPU */
				w = (sp[1] << 8) | sp[0]; sp += 2;
				dp += ((unsigned long)dp & 1);	/* Align to word boundary */
				*((uint16_t *)dp) = w; dp += 2;
				break;
			case 'd':	/* 32-bit dword, convert from little endian to CPU */
				d = (sp[3] << 24) | (sp[2] << 16) | (sp[1] << 8) | sp[0]; sp += 4;
				dp += ((unsigned long)dp & 2);	/* Align to dword boundary */
				*((uint32_t *)dp) = d; dp += 4;
				break;
			case 'W':	/* 16-bit word, keep CPU endianess */
				dp += ((unsigned long)dp & 1);	/* Align to word boundary */
				memcpy(dp, sp, 2); sp += 2; dp += 2;
				break;
			case 'D':	/* 32-bit dword, keep CPU endianess */
				dp += ((unsigned long)dp & 2);	/* Align to dword boundary */
				memcpy(dp, sp, 4); sp += 4; dp += 4;
				break;
		}
	}

	return sp - source;
}

static void clear_endpoint(struct libusb_endpoint_descriptor *endpoint)
{
	if (endpoint->extra)
		free((unsigned char *) endpoint->extra);
}

static int parse_endpoint(struct libusb_endpoint_descriptor *endpoint,
	unsigned char *buffer, int size)
{
	struct usb_descriptor_header header;
	unsigned char *extra;
	unsigned char *begin;
	int parsed = 0;
	int len;

	usbi_parse_descriptor(buffer, "bb", &header);

	/* Everything should be fine being passed into here, but we sanity */
	/*  check JIC */
	if (header.bLength > size) {
		usbi_err("ran out of descriptors parsing");
		return -1;
	}

	if (header.bDescriptorType != LIBUSB_DT_ENDPOINT) {
		usbi_err("unexpected descriptor %x (expected %x)",
			header.bDescriptorType, LIBUSB_DT_ENDPOINT);
		return parsed;
	}

	if (header.bLength >= ENDPOINT_AUDIO_DESC_LENGTH)
		usbi_parse_descriptor(buffer, "bbbbwbbb", endpoint);
	else if (header.bLength >= ENDPOINT_DESC_LENGTH)
		usbi_parse_descriptor(buffer, "bbbbwb", endpoint);

	buffer += header.bLength;
	size -= header.bLength;
	parsed += header.bLength;

	/* Skip over the rest of the Class Specific or Vendor Specific */
	/*  descriptors */
	begin = buffer;
	while (size >= DESC_HEADER_LENGTH) {
		usbi_parse_descriptor(buffer, "bb", &header);

		if (header.bLength < 2) {
			usbi_err("invalid descriptor length %d", header.bLength);
			return -1;
		}

		/* If we find another "proper" descriptor then we're done  */
		if ((header.bDescriptorType == LIBUSB_DT_ENDPOINT) ||
				(header.bDescriptorType == LIBUSB_DT_INTERFACE) ||
				(header.bDescriptorType == LIBUSB_DT_CONFIG) ||
				(header.bDescriptorType == LIBUSB_DT_DEVICE))
			break;

		usbi_dbg("skipping descriptor %x", header.bDescriptorType);
		buffer += header.bLength;
		size -= header.bLength;
		parsed += header.bLength;
	}

	/* Copy any unknown descriptors into a storage area for drivers */
	/*  to later parse */
	len = (int)(buffer - begin);
	if (!len) {
		endpoint->extra = NULL;
		endpoint->extra_length = 0;
		return parsed;
	}

	extra = malloc(len);
	endpoint->extra = extra;
	if (!extra) {
		endpoint->extra_length = 0;
		return LIBUSB_ERROR_NO_MEM;
	}

	memcpy(extra, begin, len);
	endpoint->extra_length = len;

	return parsed;
}

static void clear_interface(struct libusb_interface *interface)
{
	int i;
	int j;

	if (interface->altsetting) {
		for (i = 0; i < interface->num_altsetting; i++) {
			struct libusb_interface_descriptor *ifp =
				(struct libusb_interface_descriptor *)
				interface->altsetting + i;
			if (ifp->extra)
				free((void *) ifp->extra);
			if (ifp->endpoint) {
				for (j = 0; j < ifp->bNumEndpoints; j++)
					clear_endpoint((struct libusb_endpoint_descriptor *)
						ifp->endpoint + j);
				free((void *) ifp->endpoint);
			}
		}
		free((void *) interface->altsetting);
	}
	
}

static int parse_interface(struct libusb_interface *interface,
	unsigned char *buffer, int size)
{
	int i;
	int len;
	int r;
	int parsed = 0;
	int tmp;
	struct usb_descriptor_header header;
	struct libusb_interface_descriptor *ifp;
	unsigned char *begin;

	interface->num_altsetting = 0;

	while (size >= INTERFACE_DESC_LENGTH) {
		struct libusb_interface_descriptor *altsetting =
			(struct libusb_interface_descriptor *) interface->altsetting;
		altsetting = realloc(altsetting,
			sizeof(struct libusb_interface_descriptor) *
			(interface->num_altsetting + 1));
		if (!altsetting) {
			r = LIBUSB_ERROR_NO_MEM;
			goto err;
		}
		interface->altsetting = altsetting;

		ifp = altsetting + interface->num_altsetting;
		interface->num_altsetting++;
		usbi_parse_descriptor(buffer, "bbbbbbbbb", ifp);
		ifp->extra = NULL;
		ifp->extra_length = 0;
		ifp->endpoint = NULL;

		/* Skip over the interface */
		buffer += ifp->bLength;
		parsed += ifp->bLength;
		size -= ifp->bLength;

		begin = buffer;

		/* Skip over any interface, class or vendor descriptors */
		while (size >= DESC_HEADER_LENGTH) {
			usbi_parse_descriptor(buffer, "bb", &header);
			if (header.bLength < 2) {
				usbi_err("invalid descriptor of length %d", header.bLength);
				r = LIBUSB_ERROR_IO;
				goto err;
			}

			/* If we find another "proper" descriptor then we're done */
			if ((header.bDescriptorType == LIBUSB_DT_INTERFACE) ||
					(header.bDescriptorType == LIBUSB_DT_ENDPOINT) ||
					(header.bDescriptorType == LIBUSB_DT_CONFIG) ||
					(header.bDescriptorType == LIBUSB_DT_DEVICE))
				break;

			buffer += header.bLength;
			parsed += header.bLength;
			size -= header.bLength;
		}

		/* Copy any unknown descriptors into a storage area for */
		/*  drivers to later parse */
		len = (int)(buffer - begin);
		if (len) {
			ifp->extra = malloc(len);
			if (!ifp->extra) {
				r = LIBUSB_ERROR_NO_MEM;
				goto err;
			}
			memcpy((unsigned char *) ifp->extra, begin, len);
			ifp->extra_length = len;
		}

		/* Did we hit an unexpected descriptor? */
		usbi_parse_descriptor(buffer, "bb", &header);
		if ((size >= DESC_HEADER_LENGTH) &&
				((header.bDescriptorType == LIBUSB_DT_CONFIG) ||
				 (header.bDescriptorType == LIBUSB_DT_DEVICE)))
			return parsed;

		if (ifp->bNumEndpoints > USB_MAXENDPOINTS) {
			usbi_err("too many endpoints (%d)", ifp->bNumEndpoints);
			r = LIBUSB_ERROR_IO;
			goto err;
		}

		if (ifp->bNumEndpoints > 0) {
			struct libusb_endpoint_descriptor *endpoint;
			tmp = ifp->bNumEndpoints * sizeof(struct libusb_endpoint_descriptor);
			endpoint = malloc(tmp);
			ifp->endpoint = endpoint;
			if (!endpoint) {
				r = LIBUSB_ERROR_NO_MEM;
				goto err;
			}

			memset(endpoint, 0, tmp);
			for (i = 0; i < ifp->bNumEndpoints; i++) {
				usbi_parse_descriptor(buffer, "bb", &header);

				if (header.bLength > size) {
					usbi_err("ran out of descriptors parsing");
					r = LIBUSB_ERROR_IO;
					goto err;
				}

				r = parse_endpoint(endpoint + i, buffer, size);
				if (r < 0)
					goto err;

				buffer += r;
				parsed += r;
				size -= r;
			}
		}

		/* We check to see if it's an alternate to this one */
		ifp = (struct libusb_interface_descriptor *) buffer;
		if (size < LIBUSB_DT_INTERFACE_SIZE ||
				ifp->bDescriptorType != LIBUSB_DT_INTERFACE ||
				!ifp->bAlternateSetting)
			return parsed;
	}

	return parsed;
err:
	clear_interface(interface);
	return r;
}

static void clear_configuration(struct libusb_config_descriptor *config)
{
	if (config->interface) {
		int i;
		for (i = 0; i < config->bNumInterfaces; i++)
			clear_interface((struct libusb_interface *)
				config->interface + i);
		free((void *) config->interface);
	}
	if (config->extra)
		free((void *) config->extra);
}

static int parse_configuration(struct libusb_config_descriptor *config,
	unsigned char *buffer)
{
	int i;
	int r;
	int size;
	int tmp;
	struct usb_descriptor_header header;
	struct libusb_interface *interface;

	usbi_parse_descriptor(buffer, "bbwbbbbb", config);
	size = config->wTotalLength;

	if (config->bNumInterfaces > USB_MAXINTERFACES) {
		usbi_err("too many interfaces (%d)", config->bNumInterfaces);
		return LIBUSB_ERROR_IO;
	}

	tmp = config->bNumInterfaces * sizeof(struct libusb_interface);
	interface = malloc(tmp);
	config->interface = interface;
	if (!config->interface)
		return LIBUSB_ERROR_NO_MEM;

	memset(interface, 0, tmp);
	buffer += config->bLength;
	size -= config->bLength;

	config->extra = NULL;
	config->extra_length = 0;

	for (i = 0; i < config->bNumInterfaces; i++) {
		int len;
		unsigned char *begin;

		/* Skip over the rest of the Class Specific or Vendor */
		/*  Specific descriptors */
		begin = buffer;
		while (size >= DESC_HEADER_LENGTH) {
			usbi_parse_descriptor(buffer, "bb", &header);

			if ((header.bLength > size) ||
					(header.bLength < DESC_HEADER_LENGTH)) {
				usbi_err("invalid descriptor length of %d", header.bLength);
				r = LIBUSB_ERROR_IO;
				goto err;
			}

			/* If we find another "proper" descriptor then we're done */
			if ((header.bDescriptorType == LIBUSB_DT_ENDPOINT) ||
					(header.bDescriptorType == LIBUSB_DT_INTERFACE) ||
					(header.bDescriptorType == LIBUSB_DT_CONFIG) ||
					(header.bDescriptorType == LIBUSB_DT_DEVICE))
				break;

			usbi_dbg("skipping descriptor 0x%x\n", header.bDescriptorType);
			buffer += header.bLength;
			size -= header.bLength;
		}

		/* Copy any unknown descriptors into a storage area for */
		/*  drivers to later parse */
		len = (int)(buffer - begin);
		if (len) {
			/* FIXME: We should realloc and append here */
			if (!config->extra_length) {
				config->extra = malloc(len);
				if (!config->extra) {
					r = LIBUSB_ERROR_NO_MEM;
					goto err;
				}

				memcpy((unsigned char *) config->extra, begin, len);
				config->extra_length = len;
			}
		}

		r = parse_interface(interface + i, buffer, size);
		if (r < 0)
			goto err;

		buffer += r;
		size -= r;
	}

	return size;

err:
	clear_configuration(config);
	return r;
}

/** \ingroup desc
 * Get the USB device descriptor for a given device.
 *
 * This is a non-blocking function; the device descriptor is cached in memory.
 *
 * \param dev the device
 * \param desc output location for the descriptor data
 * \returns 0 on success or a LIBUSB_ERROR code on failure
 */
API_EXPORTED int libusb_get_device_descriptor(libusb_device *dev,
	struct libusb_device_descriptor *desc)
{
	unsigned char raw_desc[DEVICE_DESC_LENGTH];
	int r;

	usbi_dbg("");
	r = usbi_backend->get_device_descriptor(dev, raw_desc);
	if (r < 0)
		return r;

	usbi_parse_descriptor(raw_desc, "bbWbbbbWWWbbbb", desc);
	return 0;
}

/** \ingroup desc
 * Get the USB configuration descriptor for the currently active configuration.
 * This is a non-blocking function which does not involve any requests being
 * sent to the device.
 *
 * \param dev a device
 * \returns the USB configuration descriptor which must be freed with
 * libusb_free_config_descriptor() when done
 * \returns NULL on error
 * \see libusb_get_config_descriptor
 */
API_EXPORTED
struct libusb_config_descriptor *libusb_get_active_config_descriptor(
	libusb_device *dev)
{
	struct libusb_config_descriptor *config = malloc(sizeof(*config));
	unsigned char tmp[8];
	unsigned char *buf = NULL;
	int r;

	usbi_dbg("");
	if (!config)
		return NULL;

	r = usbi_backend->get_active_config_descriptor(dev, tmp, sizeof(tmp));
	if (r < 0)
		goto err;

	usbi_parse_descriptor(tmp, "bbw", config);
	buf = malloc(config->wTotalLength);
	if (!buf)
		goto err;

	r = usbi_backend->get_active_config_descriptor(dev, buf,
		config->wTotalLength);
	if (r < 0)
		goto err;

	r = parse_configuration(config, buf);
	if (r < 0) {
		usbi_err("parse_configuration failed with error %d", r);
		goto err;
	} else if (r > 0) {
		usbi_warn("descriptor data still left");
	}

	return config;

err:
	free(config);
	if (buf)
		free(buf);
	return NULL;
}

/** \ingroup desc
 * Get a USB configuration descriptor based on its index.
 * This is a non-blocking function which does not involve any requests being
 * sent to the device.
 *
 * \param dev a device
 * \param config_index the index of the configuration you wish to retrieve
 * \returns the USB configuration descriptor which must be freed with
 * libusb_free_config_descriptor() when done
 * \returns NULL on error
 * \see libusb_get_active_config_descriptor()
 * \see libusb_get_config_descriptor_by_value()
 */
API_EXPORTED struct libusb_config_descriptor *libusb_get_config_descriptor(
	libusb_device *dev, uint8_t config_index)
{
	struct libusb_config_descriptor *config;
	unsigned char tmp[8];
	unsigned char *buf = NULL;
	int r;

	usbi_dbg("index %d", config_index);
	if (config_index >= dev->num_configurations)
		return NULL;

	config = malloc(sizeof(*config));
	if (!config)
		return NULL;

	r = usbi_backend->get_config_descriptor(dev, config_index, tmp,
		sizeof(tmp));
	if (r < 0)
		goto err;

	usbi_parse_descriptor(tmp, "bbw", config);
	buf = malloc(config->wTotalLength);
	if (!buf)
		goto err;

	r = usbi_backend->get_config_descriptor(dev, config_index, buf,
		config->wTotalLength);
	if (r < 0)
		goto err;

	r = parse_configuration(config, buf);
	if (r < 0) {
		usbi_err("parse_configuration failed with error %d", r);
		goto err;
	} else if (r > 0) {
		usbi_warn("descriptor data still left");
	}

	return config;

err:
	free(config);
	if (buf)
		free(buf);
	return NULL;
}

/** \ingroup desc
 * Get a USB configuration descriptor with a specific bConfigurationValue.
 * This is a non-blocking function which does not involve any requests being
 * sent to the device.
 *
 * \param dev a device
 * \param bConfigurationValue the bConfigurationValue of the configuration you
 * wish to retrieve
 * \returns the USB configuration descriptor which must be freed with
 * libusb_free_config_descriptor() when done
 * \returns NULL on error
 * \see libusb_get_active_config_descriptor()
 * \see libusb_get_config_descriptor()
 */
API_EXPORTED
struct libusb_config_descriptor *libusb_get_config_descriptor_by_value(
	libusb_device *dev, uint8_t bConfigurationValue)
{
	int i;
	int r;
	int found = -1;

	usbi_dbg("value %d", bConfigurationValue);
	for (i = 0; i < dev->num_configurations; i++) {
		unsigned char tmp[6];
		r = usbi_backend->get_config_descriptor(dev, i, tmp, sizeof(tmp));
		if (r < 0)
			return NULL;
		if (tmp[5] == bConfigurationValue) {
			found = 1;
			break;
		}
	}

	if (!found)
		return NULL;
	else
		return libusb_get_config_descriptor(dev, i);
}

/** \ingroup desc
 * Free a configuration descriptor obtained from
 * libusb_get_active_config_descriptor() or libusb_get_config_descriptor().
 * It is safe to call this function with a NULL config parameter, in which
 * case the function simply returns.
 *
 * \param config the configuration descriptor to free
 */
API_EXPORTED void libusb_free_config_descriptor(
	struct libusb_config_descriptor *config)
{
	if (!config)
		return;

	clear_configuration(config);
	free(config);
}

/** \ingroup desc
 * Retrieve a string descriptor in C style ASCII.
 *
 * Wrapper around libusb_get_string_descriptor(). Uses the first language 
 * supported by the device.
 *
 * \param dev a device handle
 * \param desc_index the index of the descriptor to retrieve
 * \param data output buffer for ASCII string descriptor
 * \param length size of data buffer
 * \returns number of bytes returned in data, or LIBUSB_ERROR code on failure
 */
API_EXPORTED int libusb_get_string_descriptor_ascii(libusb_device_handle *dev,
	uint8_t desc_index, unsigned char *data, int length)
{
	unsigned char tbuf[255]; /* Some devices choke on size > 255 */
	int r, langid, si, di;

	/* Asking for the zero'th index is special - it returns a string
	 * descriptor that contains all the language IDs supported by the device.
	 * Typically there aren't many - often only one. The language IDs are 16
	 * bit numbers, and they start at the third byte in the descriptor. See
	 * USB 2.0 specification section 9.6.7 for more information. */
	r = libusb_get_string_descriptor(dev, 0, 0, tbuf, sizeof(tbuf));
	if (r < 0)
		return r;

	if (r < 4)
		return LIBUSB_ERROR_IO;

	langid = tbuf[2] | (tbuf[3] << 8);

	r = libusb_get_string_descriptor(dev, desc_index, langid, tbuf,
		sizeof(tbuf));
	if (r < 0)
		return r;

	if (tbuf[1] != LIBUSB_DT_STRING)
		return LIBUSB_ERROR_IO;

	if (tbuf[0] > r)
		return LIBUSB_ERROR_IO;

	for (di = 0, si = 2; si < tbuf[0]; si += 2) {
		if (di >= (length - 1))
			break;

		if (tbuf[si + 1]) /* high byte */
			data[di++] = '?';
		else
			data[di++] = tbuf[si];
	}

	data[di] = 0;
	return di;
}

