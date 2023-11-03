// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015-2018, Intel Corporation.
 * Copyright (c) 2021, IBM Corp.
 */

#include <linux/device.h>
#include <linux/kcs_bmc.h>
#include <linux/kcs_bmc_client.h>
#include <linux/kcs_bmc_device.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>


/* Record registered devices and drivers */
static DEFINE_MUTEX(kcs_bmc_lock);
static LIST_HEAD(kcs_bmc_devices);
static LIST_HEAD(kcs_bmc_drivers);
static LIST_HEAD(kcs_bmc_clients);

/* Consumer data access */

static void kcs_bmc_client_validate(struct kcs_bmc_client *client)
{
	WARN_ONCE(client != READ_ONCE(client->dev->client), "KCS client confusion detected");
}

u8 kcs_bmc_read_data(struct kcs_bmc_client *client)
{
	struct kcs_bmc_device *dev = client->dev;

	kcs_bmc_client_validate(client);
	return dev->ops->io_inputb(dev, dev->ioreg.idr);
}
EXPORT_SYMBOL(kcs_bmc_read_data);

void kcs_bmc_write_data(struct kcs_bmc_client *client, u8 data)
{
	struct kcs_bmc_device *dev = client->dev;

	kcs_bmc_client_validate(client);
	dev->ops->io_outputb(dev, dev->ioreg.odr, data);
}
EXPORT_SYMBOL(kcs_bmc_write_data);

u8 kcs_bmc_read_status(struct kcs_bmc_client *client)
{
	struct kcs_bmc_device *dev = client->dev;

	kcs_bmc_client_validate(client);
	return dev->ops->io_inputb(dev, dev->ioreg.str);
}
EXPORT_SYMBOL(kcs_bmc_read_status);

void kcs_bmc_write_status(struct kcs_bmc_client *client, u8 data)
{
	struct kcs_bmc_device *dev = client->dev;

	kcs_bmc_client_validate(client);
	dev->ops->io_outputb(dev, dev->ioreg.str, data);
}
EXPORT_SYMBOL(kcs_bmc_write_status);

void kcs_bmc_update_status(struct kcs_bmc_client *client, u8 mask, u8 val)
{
	struct kcs_bmc_device *dev = client->dev;

	kcs_bmc_client_validate(client);
	dev->ops->io_updateb(dev, dev->ioreg.str, mask, val);
}
EXPORT_SYMBOL(kcs_bmc_update_status);

irqreturn_t kcs_bmc_handle_event(struct kcs_bmc_device *kcs_bmc)
{
	struct kcs_bmc_client *client;
	irqreturn_t rc = IRQ_NONE;
	unsigned long flags;

	spin_lock_irqsave(&kcs_bmc->lock, flags);
	client = kcs_bmc->client;
	if (client)
		rc = client->ops->event(client);
	spin_unlock_irqrestore(&kcs_bmc->lock, flags);

	return rc;
}
EXPORT_SYMBOL(kcs_bmc_handle_event);

static void kcs_bmc_update_event_mask(struct kcs_bmc_device *kcs_bmc, u8 mask, u8 events)
{
	kcs_bmc->ops->irq_mask_update(kcs_bmc, mask, events);
}

int kcs_bmc_enable_device(struct kcs_bmc_client *client)
{
	struct kcs_bmc_device *dev = client->dev;
	int rc;

	spin_lock_irq(&dev->lock);
	if (dev->client) {
		rc = -EBUSY;
	} else {
		u8 mask = KCS_BMC_EVENT_TYPE_IBF;

		dev->client = client;
		kcs_bmc_update_event_mask(dev, mask, mask);
		rc = 0;
	}
	spin_unlock_irq(&dev->lock);

	return rc;
}
EXPORT_SYMBOL(kcs_bmc_enable_device);

void kcs_bmc_disable_device(struct kcs_bmc_client *client)
{
	struct kcs_bmc_device *dev = client->dev;

	spin_lock_irq(&dev->lock);
	if (client == dev->client) {
		u8 mask = KCS_BMC_EVENT_TYPE_IBF | KCS_BMC_EVENT_TYPE_OBE;

		kcs_bmc_update_event_mask(dev, mask, 0);
		dev->client = NULL;
	}
	spin_unlock_irq(&dev->lock);
}
EXPORT_SYMBOL(kcs_bmc_disable_device);

int kcs_bmc_add_device(struct kcs_bmc_device *dev)
{
	struct kcs_bmc_client *client;
	struct kcs_bmc_driver *drv;
	int error = 0;

	spin_lock_init(&dev->lock);
	dev->client = NULL;

	mutex_lock(&kcs_bmc_lock);
	list_add(&dev->entry, &kcs_bmc_devices);
	list_for_each_entry(drv, &kcs_bmc_drivers, entry) {
		client = drv->ops->add_device(drv, dev);
		if (IS_ERR(client)) {
			error = PTR_ERR(client);
			dev_err(dev->dev,
				"Failed to add chardev for KCS channel %d: %d",
				dev->channel, error);
			break;
		}
		list_add(&client->entry, &kcs_bmc_clients);
	}
	mutex_unlock(&kcs_bmc_lock);

	return error;
}
EXPORT_SYMBOL(kcs_bmc_add_device);

void kcs_bmc_remove_device(struct kcs_bmc_device *dev)
{
	struct kcs_bmc_client *curr, *tmp;

	mutex_lock(&kcs_bmc_lock);
	list_for_each_entry_safe(curr, tmp, &kcs_bmc_clients, entry) {
		if (curr->dev == dev) {
			list_del(&curr->entry);
			curr->drv->ops->remove_device(curr);
		}
	}
	list_del(&dev->entry);
	mutex_unlock(&kcs_bmc_lock);
}
EXPORT_SYMBOL(kcs_bmc_remove_device);

int kcs_bmc_register_driver(struct kcs_bmc_driver *drv)
{
	struct kcs_bmc_client *client;
	struct kcs_bmc_device *dev;

	mutex_lock(&kcs_bmc_lock);
	list_add(&drv->entry, &kcs_bmc_drivers);
	list_for_each_entry(dev, &kcs_bmc_devices, entry) {
		client = drv->ops->add_device(drv, dev);
		if (IS_ERR(client)) {
			dev_err(dev->dev, "Failed to add driver for KCS channel %d: %ld",
				dev->channel, PTR_ERR(client));
			continue;
		}
		list_add(&client->entry, &kcs_bmc_clients);
	}
	mutex_unlock(&kcs_bmc_lock);

	return 0;
}
EXPORT_SYMBOL(kcs_bmc_register_driver);

void kcs_bmc_unregister_driver(struct kcs_bmc_driver *drv)
{
	struct kcs_bmc_client *curr, *tmp;

	mutex_lock(&kcs_bmc_lock);
	list_for_each_entry_safe(curr, tmp, &kcs_bmc_clients, entry) {
		if (curr->drv == drv) {
			list_del(&curr->entry);
			drv->ops->remove_device(curr);
		}
	}
	list_del(&drv->entry);
	mutex_unlock(&kcs_bmc_lock);
}
EXPORT_SYMBOL(kcs_bmc_unregister_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Haiyue Wang <haiyue.wang@linux.intel.com>");
MODULE_AUTHOR("Andrew Jeffery <andrew@aj.id.au>");
MODULE_DESCRIPTION("Subsystem for BMCs to communicate via KCS devices");
