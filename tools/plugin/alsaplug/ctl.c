// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2022 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>

#include <stdio.h>
#include <sys/poll.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <assert.h>
#include <errno.h>

// TODO remove parsing and read ctls from sof-pipe SHM glb context
#include <ipc/control.h>

#include <alsa/asoundlib.h>
#include <alsa/control_external.h>

#include "plugin.h"
#include "common.h"

typedef struct snd_sof_ctl {
	struct plug_shm_glb_state *glb;
	snd_ctl_ext_t ext;
	struct plug_socket_desc ipc;
	struct plug_shm_desc shm_ctx;
	int subscribed;
	int updated[MAX_CTLS];

} snd_sof_ctl_t;

#define CTL_GET_TPLG_HDR(_ctl, _key) \
		(&_ctl->glb->ctl[_key].mixer_ctl.hdr)

#define CTL_GET_TPLG_MIXER(_ctl, _key) \
		(&_ctl->glb->ctl[_key].mixer_ctl)

#define CTL_GET_TPLG_ENUM(_ctl, _key) \
		(&_ctl->glb->ctl[_key].enum_ctl)

#define CTL_GET_TPLG_BYTES(_ctl, _key) \
		(&_ctl->glb->ctl[_key].bytes_ctl)

static uint32_t mixer_to_ipc(unsigned int value, uint32_t *volume_table, int size)
{
	if (value >= size)
		return volume_table[size - 1];

	return volume_table[value];
}

static uint32_t ipc_to_mixer(uint32_t value, uint32_t *volume_table, int size)
{
	int i;

	for (i = 0; i < size; i++) {
		if (volume_table[i] >= value)
			return i;
	}

	return i - 1;
}

/* number of ctls */
static int plug_ctl_elem_count(snd_ctl_ext_t *ext)
{
	snd_sof_ctl_t *ctl = ext->private_data;

	/* TODO: get count of elems from topology */
	return ctl->glb->num_ctls;
}

static int plug_ctl_elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	struct snd_soc_tplg_ctl_hdr *hdr;

	if (offset >=  ctl->glb->num_ctls)
		return -EINVAL;

	hdr = CTL_GET_TPLG_HDR(ctl, offset);

	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, hdr->name);

	return 0;
}

static snd_ctl_ext_key_t plug_ctl_find_elem(snd_ctl_ext_t *ext, const snd_ctl_elem_id_t *id)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	unsigned int numid;

	numid = snd_ctl_elem_id_get_numid(id);

	if (numid > ctl->glb->num_ctls)
		return SND_CTL_EXT_KEY_NOT_FOUND;

	return numid - 1;
}

static int plug_ctl_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
				  int *type, unsigned int *acc, unsigned int *count)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	struct snd_soc_tplg_ctl_hdr *hdr = CTL_GET_TPLG_HDR(ctl, key);
	struct snd_soc_tplg_mixer_control *mixer_ctl;
	struct snd_soc_tplg_enum_control *enum_ctl;
	struct snd_soc_tplg_bytes_control *bytes_ctl;
	int err = 0;

	switch (hdr->ops.info) {
	case SND_SOC_TPLG_CTL_VOLSW:
	case SND_SOC_TPLG_CTL_VOLSW_SX:
	case SND_SOC_TPLG_CTL_VOLSW_XR_SX:
		mixer_ctl = (struct snd_soc_tplg_mixer_control *)hdr;

		/* check for type - boolean should be binary values */
		if (mixer_ctl->max == 1 && mixer_ctl->min == 0)
			*type = SND_CTL_ELEM_TYPE_BOOLEAN;
		else
			*type = SND_CTL_ELEM_TYPE_INTEGER;
		*count = 2;//mixer_ctl->num_channels; ///// WRONG is 0 !!!

		//printf("mixer %d %d\n", __LINE__, mixer_ctl->num_channels);
		break;
	case SND_SOC_TPLG_CTL_ENUM:
	case SND_SOC_TPLG_CTL_ENUM_VALUE:
		enum_ctl = (struct snd_soc_tplg_enum_control *)hdr;
		*type = SND_CTL_ELEM_TYPE_ENUMERATED;
		*count = enum_ctl->items;
		break;
	case SND_SOC_TPLG_CTL_RANGE:
	case SND_SOC_TPLG_CTL_STROBE:
		// TODO: ??
		break;
	case SND_SOC_TPLG_CTL_BYTES:
		bytes_ctl = (struct snd_soc_tplg_bytes_control *)hdr;
		*type = SND_CTL_ELEM_TYPE_BYTES;
		*count = bytes_ctl->max;
		break;
	}

	*acc = hdr->access;

	/* access needs the callback to decode the data */
	if ((hdr->access & SND_CTL_EXT_ACCESS_TLV_READ) ||
	    (hdr->access & SND_CTL_EXT_ACCESS_TLV_WRITE))
		*acc |= SND_CTL_EXT_ACCESS_TLV_CALLBACK;
	return err;
}

/*
 * Integer ops
 */
static int plug_ctl_get_integer_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *imin,
				     long *imax, long *istep)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	struct snd_soc_tplg_ctl_hdr *hdr = CTL_GET_TPLG_HDR(ctl, key);
	struct snd_soc_tplg_mixer_control *mixer_ctl =
			CTL_GET_TPLG_MIXER(ctl, key);
	int err = 0;

	switch (hdr->type) {
	case SND_SOC_TPLG_CTL_VOLSW:
	case SND_SOC_TPLG_CTL_VOLSW_SX:
	case SND_SOC_TPLG_CTL_VOLSW_XR_SX:
		/* TLV uses the fields differently */
		if ((hdr->access & SND_CTL_EXT_ACCESS_TLV_READ) ||
		    (hdr->access & SND_CTL_EXT_ACCESS_TLV_WRITE)) {
			*istep = mixer_ctl->hdr.tlv.scale.step;
			*imin = (int32_t)mixer_ctl->hdr.tlv.scale.min;
			*imax = mixer_ctl->max;
		} else {
			*istep = 1;
			*imin = mixer_ctl->min;
			*imax = mixer_ctl->max;
		}
		break;
	default:
		SNDERR("invalid ctl type for integer using key %d", key);
		err = -EINVAL;
		break;
	}

	return err;
}

static int plug_ctl_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	struct snd_soc_tplg_mixer_control *mixer_ctl = CTL_GET_TPLG_MIXER(ctl, key);
	struct ipc4_module_large_config config = {{ 0 }};
	struct ipc4_peak_volume_config *volume;
	struct ipc4_module_large_config_reply *reply;
	char *reply_data;
	void *msg;
	int reply_data_size, size;
	int num_data_items;
	int i, err;

	/* configure the IPC message */
	plug_ctl_ipc_message(&config, IPC4_VOLUME, sizeof(volume), ctl->glb->ctl[key].module_id,
			     ctl->glb->ctl[key].instance_id, SOF_IPC4_MOD_LARGE_CONFIG_GET);

	config.extension.r.final_block = 1;
	config.extension.r.init_block = 1;

	size = sizeof(config);
	msg = calloc(size, 1);
	if (!msg)
		return -ENOMEM;

	/* reply contains both the requested data and the reply status */
	reply_data_size = sizeof(*reply) + mixer_ctl->num_channels * sizeof(*volume);
	reply_data = calloc(reply_data_size, 1);
	if (!reply_data_size) {
		free(msg);
		return -ENOMEM;
	}

	/* send the IPC message */
	memcpy(msg, &config, sizeof(config));
	err = plug_ipc_cmd_tx_rx(&ctl->ipc, msg, size, reply_data, reply_data_size);
	free(msg);
	if (err < 0) {
		SNDERR("failed to set volume for control %s\n", mixer_ctl->hdr.name);
		goto out;
	}

	reply = (struct ipc4_module_large_config_reply *)reply_data;
	if (reply->primary.r.status != IPC4_SUCCESS) {
		SNDERR("volume control %s set failed with status %d\n",
		       mixer_ctl->hdr.name, reply->primary.r.status);
		err = -EINVAL;
		goto out;
	}

	/* check data sanity */
	num_data_items = reply->extension.r.data_off_size / sizeof(*volume);
	if (num_data_items != mixer_ctl->num_channels) {
		SNDERR("Channel count %d doesn't match the expected value %d\n",
		       num_data_items, mixer_ctl->num_channels);
		err = -EINVAL;
		goto out;
	}

	/* set the mixer values based on the received data */
	volume = (struct ipc4_peak_volume_config *)(reply_data + sizeof(*reply));
	for (i = 0; i < mixer_ctl->num_channels; i++)
		value[i] = ipc_to_mixer(volume[i].target_volume, ctl->glb->ctl[key].volume_table,
					mixer_ctl->max + 1);
out:
	free(reply_data);
	return err;
}

static int plug_ctl_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	struct snd_soc_tplg_mixer_control *mixer_ctl = CTL_GET_TPLG_MIXER(ctl, key);
	struct ipc4_module_large_config config = {{ 0 }};
	struct ipc4_peak_volume_config volume;
	struct ipc4_message_reply reply;
	bool all_channels_equal = true;
	uint32_t val;
	void *msg;
	int size, err;
	int i;

	/* set data for IPC */
	val = value[0];
	for (i = 1; i < mixer_ctl->num_channels; i++) {
		if (value[i] != val) {
			all_channels_equal = false;
			break;
		}
	}

	/*
	 * if all channels have the same volume, send a single IPC, else, send individual IPCs
	 * for each channel
	 */
	for (i = 0; i < mixer_ctl->num_channels; i++) {
		if (all_channels_equal) {
			volume.channel_id = IPC4_ALL_CHANNELS_MASK;
			volume.target_volume = mixer_to_ipc(val, ctl->glb->ctl[key].volume_table,
							    mixer_ctl->max + 1);
		} else {
			volume.channel_id = i;
			volume.target_volume = mixer_to_ipc(value[i],
							    ctl->glb->ctl[key].volume_table,
							    mixer_ctl->max + 1);
		}

		/* TODO: get curve duration and type from topology */
		volume.curve_type = 1;
		volume.curve_duration = 200000;

		/* configure the IPC message */
		plug_ctl_ipc_message(&config, IPC4_VOLUME, sizeof(volume),
				     ctl->glb->ctl[key].module_id, ctl->glb->ctl[key].instance_id,
				     SOF_IPC4_MOD_LARGE_CONFIG_SET);
		config.extension.r.final_block = 1;
		config.extension.r.init_block = 1;

		size = sizeof(config) + sizeof(volume);
		msg = calloc(size, 1);
		if (!msg)
			return -ENOMEM;

		memcpy(msg, &config, sizeof(config));
		memcpy(msg + sizeof(config), &volume, sizeof(volume));

		/* send the message and check status */
		err = plug_ipc_cmd_tx_rx(&ctl->ipc, msg, size, &reply, sizeof(reply));
		free(msg);
		if (err < 0) {
			SNDERR("failed to set volume control %s\n", mixer_ctl->hdr.name);
			return err;
		}

		if (reply.primary.r.status != IPC4_SUCCESS) {
			SNDERR("volume control %s set failed with status %d\n",
			       mixer_ctl->hdr.name, reply.primary.r.status);
			return -EINVAL;
		}

		if (all_channels_equal)
			break;
	}

	return 0;
}

/*
 * Enum ops
 */
static int plug_ctl_get_enumerated_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
					unsigned int *items)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	struct snd_soc_tplg_ctl_hdr *hdr = CTL_GET_TPLG_HDR(ctl, key);
	struct snd_soc_tplg_enum_control *enum_ctl =
			(struct snd_soc_tplg_enum_control *)hdr;
	int err = 0;

	switch (hdr->ops.info) {
	case SND_SOC_TPLG_CTL_ENUM:
	case SND_SOC_TPLG_CTL_ENUM_VALUE:
		*items = enum_ctl->items;
		break;
	default:
		SNDERR("invalid ctl type for enum using key %d", key);
		err = -EINVAL;
		break;
	}

	return err;
}

static int plug_ctl_get_enumerated_name(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
					unsigned int item, char *name, size_t name_max_len)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	struct snd_soc_tplg_ctl_hdr *hdr = CTL_GET_TPLG_HDR(ctl, key);
	struct snd_soc_tplg_enum_control *enum_ctl =
			(struct snd_soc_tplg_enum_control *)hdr;

	if (item >= enum_ctl->items) {
		SNDERR("invalid item %d for enum using key %d\n", item, key);
		return -EINVAL;
	}

	strncpy(name, enum_ctl->texts[item], name_max_len);
	return 0;
}

static int plug_ctl_read_enumerated(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
				    unsigned int *items)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	struct snd_soc_tplg_enum_control *enum_ctl = CTL_GET_TPLG_ENUM(ctl, key);
	struct ipc4_module_large_config config = {{ 0 }};
	struct ipc4_module_large_config_reply *reply;
	struct sof_ipc4_control_msg_payload *data;
	char *reply_data;
	void *msg;
	int size, reply_data_size;
	int i, err;

	/* configure the IPC message */
	plug_ctl_ipc_message(&config, SOF_IPC4_ENUM_CONTROL_PARAM_ID, 0,
			     ctl->glb->ctl[key].module_id, ctl->glb->ctl[key].instance_id,
			     SOF_IPC4_MOD_LARGE_CONFIG_GET);

	config.extension.r.final_block = 1;
	config.extension.r.init_block = 1;

	size = sizeof(config);
	msg = calloc(size, 1);
	if (!msg)
		return -ENOMEM;

	/* reply contains both the requested data and the reply status */
	reply_data_size = sizeof(*reply) + sizeof(*data) +
		enum_ctl->num_channels * sizeof(data->chanv[0]);
	reply_data = calloc(reply_data_size, 1);
	if (!reply_data_size) {
		free(msg);
		return -ENOMEM;
	}

	/* send the IPC message */
	memcpy(msg, &config, sizeof(config));
	err = plug_ipc_cmd_tx_rx(&ctl->ipc, msg, size, reply_data, reply_data_size);
	free(msg);
	if (err < 0) {
		SNDERR("failed to get enum items for control %s\n", enum_ctl->hdr.name);
		goto out;
	}

	reply = (struct ipc4_module_large_config_reply *)reply_data;
	if (reply->primary.r.status != IPC4_SUCCESS) {
		SNDERR("enum control %s get failed with status %d\n",
		       enum_ctl->hdr.name, reply->primary.r.status);
		err = -EINVAL;
		goto out;
	}

	/* check data sanity */
	data = (struct sof_ipc4_control_msg_payload *)(reply_data + sizeof(*reply));
	if (data->num_elems != enum_ctl->num_channels) {
		SNDERR("Channel count %d doesn't match the expected value %d for enum ctl %s\n",
		       data->num_elems, enum_ctl->num_channels, enum_ctl->hdr.name);
		err = -EINVAL;
		goto out;
	}

	/* set the enum items based on the received data */
	for (i = 0; i < enum_ctl->num_channels; i++)
		items[i] = data->chanv[i].value;
out:
	free(reply_data);
	return 0;
}

static int plug_ctl_write_enumerated(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
				     unsigned int *items)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	struct snd_soc_tplg_enum_control *enum_ctl = CTL_GET_TPLG_ENUM(ctl, key);
	struct ipc4_module_large_config config = {{ 0 }};
	struct sof_ipc4_control_msg_payload *data;
	struct ipc4_message_reply reply;
	void *msg;
	int data_size, msg_size;
	int err, i;

	/* size of control data */
	data_size = enum_ctl->num_channels * sizeof(struct sof_ipc4_ctrl_value_chan) +
			sizeof(*data);

	/* allocate memory for control data */
	data = calloc(data_size, 1);
	if (!data)
		return -ENOMEM;

	/* set param ID and number of channels */
	data->id = ctl->glb->ctl[key].index;
	data->num_elems = enum_ctl->num_channels;

	/* set the enum values */
	for (i = 0; i < data->num_elems; i++) {
		data->chanv[i].channel = i;
		data->chanv[i].value = items[i];
	}

	/* configure the IPC message */
	plug_ctl_ipc_message(&config, SOF_IPC4_ENUM_CONTROL_PARAM_ID, data_size,
			     ctl->glb->ctl[key].module_id, ctl->glb->ctl[key].instance_id,
			     SOF_IPC4_MOD_LARGE_CONFIG_SET);

	/*
	 * enum controls can have a maximum of 16 texts/values. So the entire data can be sent
	 * in a single IPC message
	 */
	config.extension.r.final_block = 1;
	config.extension.r.init_block = 1;

	/* allocate memory for IPC message */
	msg_size = sizeof(config) + data_size;
	msg = calloc(msg_size, 1);
	if (!msg) {
		free(data);
		return -ENOMEM;
	}

	/* set the IPC message data */
	memcpy(msg, &config, sizeof(config));
	memcpy(msg + sizeof(config), data, data_size);
	free(data);

	/* send the message and check status */
	err = plug_ipc_cmd_tx_rx(&ctl->ipc, msg, msg_size, &reply, sizeof(reply));
	free(msg);
	if (err < 0) {
		SNDERR("failed to set enum control %s\n", enum_ctl->hdr.name);
		return err;
	}

	if (reply.primary.r.status != IPC4_SUCCESS) {
		SNDERR("enum control %s set failed with status %d\n",
		       enum_ctl->hdr.name, reply.primary.r.status);
		return -EINVAL;
	}

	return 0;
}

/*
 * Bytes ops
 */
static int plug_ctl_get_bytes_data(snd_sof_ctl_t *ctl, snd_ctl_ext_key_t key,
				   struct sof_abi_hdr *abi, unsigned int max_bytes)
{
	struct snd_soc_tplg_bytes_control *bytes_ctl = CTL_GET_TPLG_BYTES(ctl, key);
	struct ipc4_module_large_config config = {{ 0 }};
	struct ipc4_module_large_config_reply *reply;
	char *reply_data, *data;
	void *msg;
	uint32_t data_size;
	int size, reply_data_size;
	int err;

	/* configure the IPC message */
	plug_ctl_ipc_message(&config, abi->type, 0,
			     ctl->glb->ctl[key].module_id, ctl->glb->ctl[key].instance_id,
			     SOF_IPC4_MOD_LARGE_CONFIG_GET);

	config.extension.r.final_block = 1;
	config.extension.r.init_block = 1;

	size = sizeof(config);
	msg = calloc(size, 1);
	if (!msg)
		return -ENOMEM;

	/*
	 * reply contains both the requested data and the reply status. Allocate enough memory
	 * for max data
	 */
	reply_data_size = sizeof(*reply) + sizeof(*data) + bytes_ctl->max;
	reply_data = calloc(reply_data_size, 1);
	if (!reply_data_size) {
		free(msg);
		return -ENOMEM;
	}

	/* send the IPC message */
	memcpy(msg, &config, sizeof(config));
	err = plug_ipc_cmd_tx_rx(&ctl->ipc, msg, size, reply_data, reply_data_size);
	free(msg);
	if (err < 0) {
		SNDERR("failed to get bytes data for control %s\n", bytes_ctl->hdr.name);
		goto out;
	}

	reply = (struct ipc4_module_large_config_reply *)reply_data;
	if (reply->primary.r.status != IPC4_SUCCESS) {
		SNDERR("bytes control %s get failed with status %d\n",
		       bytes_ctl->hdr.name, reply->primary.r.status);
		err = -EINVAL;
		goto out;
	}

	/* check data sanity */
	data = (char *)(reply_data + sizeof(*reply));
	data_size = reply->extension.r.data_off_size;
	if (data_size > bytes_ctl->max) {
		SNDERR("received data size %d is larger than max %d for bytes control %s\n",
		       data_size, bytes_ctl->max, bytes_ctl->hdr.name);
		err = -EINVAL;
		goto out;
	}

	abi->size = data_size;

	if (data_size)
		memcpy(abi->data, data, MIN(data_size, max_bytes));

	err = data_size;
out:
	free(reply_data);
	return err;
}

static int plug_ctl_read_bytes(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
			       unsigned char *data, size_t max_bytes)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	struct sof_abi_hdr *abi = (struct sof_abi_hdr *)data;
	int data_size;

	data_size = plug_ctl_get_bytes_data(ctl, key, abi, max_bytes);
	if (data_size < 0)
		return data_size;

	return 0;
}

static int plug_ctl_write_bytes(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
				unsigned char *data, size_t max_bytes)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	struct snd_soc_tplg_bytes_control *bytes_ctl = CTL_GET_TPLG_BYTES(ctl, key);
	struct sof_abi_hdr *abi = (struct sof_abi_hdr *)data;
	int err;

	/* send IPC with kcontrol data */
	err = plug_send_bytes_data(&ctl->ipc, ctl->glb->ctl[key].module_id,
				   ctl->glb->ctl[key].instance_id, abi);
	if (err < 0) {
		SNDERR("failed to set bytes data for control %s\n", bytes_ctl->hdr.name);
		return err;
	}

	return 0;
}

/* TLV ops used for TLV bytes control callback */
/* TLV ops used for TLV bytes control callback */
static int plug_tlv_rw(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, int op_flag,
		       unsigned int numid, unsigned int *tlv, unsigned int tlv_size)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	struct snd_soc_tplg_bytes_control *bytes_ctl = CTL_GET_TPLG_BYTES(ctl, key);
	struct sof_abi_hdr *abi = (struct sof_abi_hdr *)(tlv + 2); /* skip TLV header */
	int data_size;

	/* only bytes and volume controls have tlv callback set */
	if (bytes_ctl->hdr.ops.info != SND_SOC_TPLG_CTL_BYTES) {
		struct snd_soc_tplg_mixer_control *mixer_ctl = CTL_GET_TPLG_MIXER(ctl, key);
		struct snd_soc_tplg_ctl_hdr *hdr = &mixer_ctl->hdr;
		struct snd_soc_tplg_ctl_tlv *mixer_tlv = &hdr->tlv;

		/* set the dbscale values */
		tlv[0] = SND_CTL_TLVT_DB_SCALE;
		tlv[1] = sizeof(int) * 2;
		tlv[2] = mixer_tlv->scale.min;
		tlv[3] = mixer_tlv->scale.mute << 16 | mixer_tlv->scale.step;

		return 0;
	}

	/* send IPC with kcontrol data if op_flag is > 0 else send IPC to get kcontrol data */
	if (op_flag) {
		int err;

		err = plug_send_bytes_data(&ctl->ipc, ctl->glb->ctl[key].module_id,
					   ctl->glb->ctl[key].instance_id, abi);
		if (err < 0) {
			SNDERR("failed to set bytes data for control %s\n", bytes_ctl->hdr.name);
			return err;
		}

		return 0;
	}

	/* read kcontrol data */
	data_size = plug_ctl_get_bytes_data(ctl, key, abi, tlv_size);
	if (data_size < 0)
		return data_size;

	/* set data size and numid */
	tlv[0] = numid;
	tlv[1] = data_size + sizeof(*abi);

	return 0;
}

static void plug_ctl_subscribe_events(snd_ctl_ext_t *ext, int subscribe)
{
	snd_sof_ctl_t *ctl = ext->private_data;

	ctl->subscribed = !!(subscribe & SND_CTL_EVENT_MASK_VALUE);
}

static int plug_ctl_read_event(snd_ctl_ext_t *ext, snd_ctl_elem_id_t *id,
			       unsigned int *event_mask)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	int numid;
	int err = 0;

	numid = snd_ctl_elem_id_get_numid(id);

	// TODO: we need a notify() or listening thread to take async/volatile ctl
	// notifications from sof-pipe and notify userspace via events of the ctl change.
	if (!ctl->updated[numid - 1] || !ctl->subscribed) {
		err = -EAGAIN;
		goto out;
	}

	*event_mask = SND_CTL_EVENT_MASK_VALUE;
out:
	return err;
}

static int plug_ctl_poll_revents(snd_ctl_ext_t *ext, struct pollfd *pfd,
				 unsigned int nfds, unsigned short *revents)
{
	snd_sof_ctl_t *ctl = ext->private_data;
	int i;

	*revents = 0;

	for (i = 0; i < ctl->glb->num_ctls; i++) {
		if (ctl->updated[i]) {
			*revents = POLLIN;
			break;
		}
	}

	return 0;
}

static void plug_ctl_close(snd_ctl_ext_t *ext)
{
	snd_sof_ctl_t *ctl = ext->private_data;

	/* TODO: munmap */
	close(ctl->ipc.socket_fd);
	free(ctl);
}

static const snd_ctl_ext_callback_t sof_ext_callback = {
	.elem_count = plug_ctl_elem_count,
	.elem_list = plug_ctl_elem_list,
	.find_elem = plug_ctl_find_elem,
	.get_attribute = plug_ctl_get_attribute,
	.get_integer_info = plug_ctl_get_integer_info,
	.read_integer = plug_ctl_read_integer,
	.write_integer = plug_ctl_write_integer,
	.get_enumerated_info = plug_ctl_get_enumerated_info,
	.get_enumerated_name = plug_ctl_get_enumerated_name,
	.read_enumerated = plug_ctl_read_enumerated,
	.write_enumerated = plug_ctl_write_enumerated,
	.read_bytes = plug_ctl_read_bytes,
	.write_bytes = plug_ctl_write_bytes,
	.subscribe_events = plug_ctl_subscribe_events,
	.read_event = plug_ctl_read_event,
	.poll_revents = plug_ctl_poll_revents,
	.close = plug_ctl_close,
};

SND_CTL_PLUGIN_DEFINE_FUNC(sof)
{
	snd_sof_plug_t *plug;
	int err;
	snd_sof_ctl_t *ctl;

	/* create context */
	plug = calloc(1, sizeof(*plug));
	if (!plug)
		return -ENOMEM;

	ctl = calloc(1, sizeof(*ctl));
	if (!ctl)
		return -ENOMEM;
	plug->module_prv = ctl;

	/* parse the ALSA configuration file for sof plugin */
	err = plug_parse_conf(plug, name, root, conf, true);
	if (err < 0) {
		SNDERR("failed to parse config: %s", strerror(err));
		goto error;
	}

	/* init IPC socket name */
	err = plug_socket_path_init(&ctl->ipc, "sof", "ipc", 0);
	if (err < 0) {
		SNDERR("error: invalid name for IPC tx mq %s\n", plug->tplg_file);
		goto error;
	}

	err = plug_create_client_socket(&ctl->ipc);
	if (err < 0) {
		SNDERR("failed to connect to SOF pipe IPC socket : %s", strerror(err));
		return -errno;
	}

	/* create a SHM mapping for low latency stream position */
	err = plug_shm_init(&ctl->shm_ctx, plug->tplg_file, "ctx", 0);
	if (err < 0)
		goto error;

	// TODO: make this open/close per operation for shared access
	/* create a SHM mapping for low latency stream position */
	err = plug_shm_open(&ctl->shm_ctx);
	if (err < 0)
		goto error;

	/* get global context for kcontrol lookup */
	ctl->glb = ctl->shm_ctx.addr;

	/* TODO: add some flavour to the names based on the topology */
	ctl->ext.version = SND_CTL_EXT_VERSION;
	ctl->ext.card_idx = 0;
	strncpy(ctl->ext.id, "sof", sizeof(ctl->ext.id) - 1);
	strncpy(ctl->ext.driver, "SOF plugin",
		sizeof(ctl->ext.driver) - 1);
	strncpy(ctl->ext.name, "SOF", sizeof(ctl->ext.name) - 1);
	strncpy(ctl->ext.mixername, "SOF",
		sizeof(ctl->ext.mixername) - 1);

	ctl->ext.callback = &sof_ext_callback;
	ctl->ext.private_data = ctl;
	ctl->ext.tlv.c = plug_tlv_rw;

	err = snd_ctl_ext_create(&ctl->ext, name, mode);
	if (err < 0)
		goto error;

	*handlep = ctl->ext.handle;

	return 0;

error:
	free(ctl);

	return err;
}

SND_CTL_PLUGIN_SYMBOL(sof);
