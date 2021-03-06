/*
 * Copyright IBM Corp. 2007-2014
 *
 * Authors:
 *  Dan Smith <danms@us.ibm.com>
 *  Guolian Yun <yunguol@cn.ibm.com>
 *  Jay Gagnon <grendel@linux.vnet.ibm.com>
 *  Zhengang Li <lizg@cn.ibm.com>
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
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <sys/stat.h>

#include <libcmpiutil/libcmpiutil.h>

#include "device_parsing.h"
#include "misc_util.h"
#include "xmlgen.h"
#include "../src/svpc_types.h"

#define DISK_XPATH      (xmlChar *)"/domain/devices/disk | "\
        "/domain/devices/filesystem"
#define VCPU_XPATH      (xmlChar *)"/domain/vcpu"
#define NET_XPATH       (xmlChar *)"/domain/devices/interface"
#define EMU_XPATH       (xmlChar *)"/domain/devices/emulator"
#define MEM_XPATH       (xmlChar *)"/domain/memory | /domain/currentMemory"
#define CONSOLE_XPATH   (xmlChar *)"/domain/devices/console"
/*
 * To be backward compatible, serial and console is
 * still part of the graphics.
 */
#define GRAPHICS_XPATH  (xmlChar *)"/domain/devices/graphics | "\
        "/domain/devices/console"
#define INPUT_XPATH     (xmlChar *)"/domain/devices/input"
#define CONTROLLER_XPATH (xmlChar *)"/domain/devices/controller"

#define DEFAULT_BRIDGE "xenbr0"
#define DEFAULT_NETWORK "default"

#define MAX(a,b) (((a)>(b))?(a):(b))

#define DUP_FIELD(d, s, f) do {                         \
                if ((s)->f != NULL)                     \
                        (d)->f = strdup((s)->f);        \
        } while (0);

/* Device parse function */
typedef int (*dev_parse_func_t)(xmlNode *, struct virt_device **);

static void cleanup_device_address(struct device_address *addr)
{
        int i;
        if (addr == NULL)
                return;

        CU_DEBUG("Cleanup %d addresses", addr->ct);
        for (i = 0; i < addr->ct; i++) {
                free(addr->key[i]);
                free(addr->value[i]);
        }

        free(addr->key);
        free(addr->value);
        addr->ct = 0;
}

static void cleanup_disk_device(struct disk_device *dev)
{
        if (dev == NULL)
                return;

        CU_DEBUG("Clean Disk type %s", dev->type);
        free(dev->type);
        free(dev->device);
        free(dev->driver);
        free(dev->driver_type);
        free(dev->cache);
        free(dev->source);
        free(dev->virtual_dev);
        free(dev->bus_type);
        free(dev->rawio);
        free(dev->sgio);
        free(dev->access_mode);
        cleanup_device_address(&dev->address);
}

static void cleanup_vsi_device(struct vsi_device *dev)
{
        if (dev == NULL)
                return;

        CU_DEBUG("Clean VSI type %s", dev->vsi_type);
        free(dev->vsi_type);
        free(dev->manager_id);
        free(dev->type_id);
        free(dev->type_id_version);
        free(dev->instance_id);
        free(dev->filter_ref);
        free(dev->profile_id);
}

static void cleanup_net_device(struct net_device *dev)
{
        if (dev == NULL)
                return;

        CU_DEBUG("Clean net type %s", dev->type);
        free(dev->type);
        free(dev->mac);
        free(dev->source);
        free(dev->model);
        free(dev->device);
        free(dev->net_mode);
        free(dev->filter_ref);
        free(dev->poolid);
        cleanup_vsi_device(&dev->vsi);
        cleanup_device_address(&dev->address);
}

static void cleanup_emu_device(struct emu_device *dev)
{
        if (dev == NULL)
                return;

        CU_DEBUG("Clean emu %s", dev->path);
        free(dev->path);
}

static void cleanup_vnc_device(struct graphics_device *dev)
{
        free(dev->dev.vnc.port);
        free(dev->dev.vnc.host);
        free(dev->dev.vnc.keymap);
        free(dev->dev.vnc.passwd);
}

static void cleanup_sdl_device(struct graphics_device *dev)
{
        free(dev->dev.sdl.display);
        free(dev->dev.sdl.xauth);
        free(dev->dev.sdl.fullscreen);
}

static void cleanup_graphics_device(struct graphics_device *dev)
{
        if (dev == NULL || dev->type == NULL)
                return;

        CU_DEBUG("Clean graphics type %s", dev->type);
        if (STREQC(dev->type, "sdl"))
                cleanup_sdl_device(dev);
        else
                cleanup_vnc_device(dev);

        free(dev->type);
}

static void cleanup_path_device(struct path_device *dev)
{
        if (dev == NULL)
            return;

        CU_DEBUG("Clean path device %s", dev->path);
        free(dev->path);

}

static void cleanup_unixsock_device(struct unixsock_device *dev)
{
        if (dev == NULL)
            return;

        CU_DEBUG("Clean unixsock device");
        free(dev->path);
        free(dev->mode);

}

static void cleanup_tcp_device(struct tcp_device *dev)
{
        if (dev == NULL)
            return;

        CU_DEBUG("Clean tcp device");
        free(dev->mode);
        free(dev->protocol);
        free(dev->host);
        free(dev->service);

}

static void cleanup_udp_device(struct udp_device *dev)
{
        if (dev == NULL)
            return;

        CU_DEBUG("Clean udb bind device");
        free(dev->bind_host);
        free(dev->bind_service);
        free(dev->connect_host);
        free(dev->connect_service);
};

static void cleanup_console_device(struct console_device *dev)
{
        if (dev == NULL)
                return;

        CU_DEBUG("Clean console source_type %d", dev->source_type);
        switch (dev->source_type)
        {
        case CIM_CHARDEV_SOURCE_TYPE_PTY:
                cleanup_path_device(&dev->source_dev.pty);
                break;
        case CIM_CHARDEV_SOURCE_TYPE_DEV:
                cleanup_path_device(&dev->source_dev.dev);
                break;
        case CIM_CHARDEV_SOURCE_TYPE_FILE:
                cleanup_path_device(&dev->source_dev.file);
                break;
        case CIM_CHARDEV_SOURCE_TYPE_PIPE:
                cleanup_path_device(&dev->source_dev.pipe);
                break;
        case CIM_CHARDEV_SOURCE_TYPE_UNIXSOCK:
                cleanup_unixsock_device(&dev->source_dev.unixsock);
                break;
        case CIM_CHARDEV_SOURCE_TYPE_UDP:
                cleanup_udp_device(&dev->source_dev.udp);
                break;
        case CIM_CHARDEV_SOURCE_TYPE_TCP:
                cleanup_tcp_device(&dev->source_dev.tcp);
                break;
        default:
                /* Nothing to do for :
                   CIM_CHARDEV_SOURCE_TYPE_STDIO
                   CIM_CHARDEV_SOURCE_TYPE_NULL
                   CIM_CHARDEV_SOURCE_TYPE_VC
                   CIM_CHARDEV_SOURCE_TYPE_SPICEVMC
                */
                break;
        }

        dev->source_type = 0;
        free(dev->target_type);
        memset(&dev->source_dev, 0, sizeof(dev->source_dev));
};

static void console_device_dup(struct console_device *t,
                               struct console_device *s)
{
        cleanup_console_device(t);

        t->source_type = s->source_type;
        DUP_FIELD(t, s, target_type);

        switch (s->source_type)
        {
        case CIM_CHARDEV_SOURCE_TYPE_PTY:
                DUP_FIELD(t, s, source_dev.pty.path);
                break;
        case CIM_CHARDEV_SOURCE_TYPE_DEV:
                DUP_FIELD(t, s, source_dev.dev.path);
                break;
        case CIM_CHARDEV_SOURCE_TYPE_FILE:
                DUP_FIELD(t, s, source_dev.file.path);
                break;
        case CIM_CHARDEV_SOURCE_TYPE_PIPE:
                DUP_FIELD(t, s, source_dev.pipe.path);
                break;
        case CIM_CHARDEV_SOURCE_TYPE_UNIXSOCK:
                DUP_FIELD(t, s, source_dev.unixsock.path);
                DUP_FIELD(t, s, source_dev.unixsock.mode);
                break;
        case CIM_CHARDEV_SOURCE_TYPE_UDP:
                DUP_FIELD(t, s, source_dev.udp.bind_host);
                DUP_FIELD(t, s, source_dev.udp.bind_service);
                DUP_FIELD(t, s, source_dev.udp.connect_host);
                DUP_FIELD(t, s, source_dev.udp.connect_service);
                break;
        case CIM_CHARDEV_SOURCE_TYPE_TCP:
                DUP_FIELD(t, s, source_dev.tcp.mode);
                DUP_FIELD(t, s, source_dev.tcp.protocol);
                DUP_FIELD(t, s, source_dev.tcp.host);
                DUP_FIELD(t, s, source_dev.tcp.service);
                break;
        default:
                /* Nothing to do for :
                   CIM_CHARDEV_SOURCE_TYPE_STDIO
                   CIM_CHARDEV_SOURCE_TYPE_NULL
                   CIM_CHARDEV_SOURCE_TYPE_VC
                   CIM_CHARDEV_SOURCE_TYPE_SPICEVMC
                */
                break;
        }
}

static void cleanup_input_device(struct input_device *dev)
{
        if (dev == NULL)
                return;

        CU_DEBUG("Clean input device %s", dev->type);
        free(dev->type);
        free(dev->bus);
}

static void cleanup_controller_device(struct controller_device *dev)
{
        if (dev == NULL)
                return;

        CU_DEBUG("Clean controller device %d", dev->type);
        free(dev->model);
        free(dev->queues);
        free(dev->ports);
        free(dev->vectors);
        cleanup_device_address(&dev->address);
}

void cleanup_virt_device(struct virt_device *dev)
{
        if (dev == NULL)
                return; /* free()-like semantics */

        if (dev->type == CIM_RES_TYPE_DISK)
                cleanup_disk_device(&dev->dev.disk);
        else if (dev->type == CIM_RES_TYPE_NET)
                cleanup_net_device(&dev->dev.net);
        else if (dev->type == CIM_RES_TYPE_EMU)
                cleanup_emu_device(&dev->dev.emu);
        else if (dev->type == CIM_RES_TYPE_GRAPHICS)
                cleanup_graphics_device(&dev->dev.graphics);
        else if (dev->type == CIM_RES_TYPE_INPUT)
                cleanup_input_device(&dev->dev.input);
        else if (dev->type == CIM_RES_TYPE_CONSOLE)
                cleanup_console_device(&dev->dev.console);
        else if (dev->type == CIM_RES_TYPE_CONTROLLER)
                cleanup_controller_device(&dev->dev.controller);

        free(dev->id);

        memset(dev, 0, sizeof(*dev));
}

void cleanup_virt_devices(struct virt_device **_devs, int count)
{
        int i;
        struct virt_device *devs = *_devs;

        for (i = 0; i < count; i++)
                cleanup_virt_device(&devs[i]);

        CU_DEBUG("All devices cleaned");
        free(devs);
        *_devs = NULL;
}

char *get_attr_value(xmlNode *node, char *attrname)
{
        char *buf = NULL;
        char *ret = NULL;

        buf = (char *)xmlGetProp(node, (xmlChar *)attrname);
        if (buf) {
                ret = strdup(buf);
                xmlFree(buf);
        }

        return ret;
}

char *get_node_content(xmlNode *node)
{
        char *buf = NULL;
        xmlChar *ret = NULL;

        ret = xmlNodeGetContent(node);
        if (ret) {
                buf = strdup((char *)ret);
                xmlFree(ret);
        }

        return buf;
}

int add_device_address_property(struct device_address *devaddr,
                                const char *key,
                                const char *value)
{
        char *k = NULL;
        char *v = NULL;
        char **list = NULL;

        if (key != NULL && value != NULL) {
                k = strdup(key);
                v = strdup(value);
                if (k == NULL || v == NULL)
                        goto err;

                list = realloc(devaddr->key, sizeof(char*) * (devaddr->ct+1));
                if (list == NULL)
                        goto err;
                devaddr->key = list;

                list = realloc(devaddr->value, sizeof(char*) * (devaddr->ct+1));
                if (list == NULL)
                        goto err;
                devaddr->value = list;

                devaddr->key[devaddr->ct] = k;
                devaddr->value[devaddr->ct] = v;
                devaddr->ct += 1;
                return 1;
        }

 err:
        free(k);
        free(v);
        free(list);
        return 0;
}


static int parse_device_address(xmlNode *anode, struct device_address *devaddr)
{
        xmlAttr *attr = NULL;
        char *name = NULL;
        char *value = NULL;

        for (attr = anode->properties; attr != NULL; attr = attr->next) {
                name = (char*) attr->name;
                value = get_attr_value(anode, name);
                if (!add_device_address_property(devaddr, name, value))
                        goto err;
                free(value);
        }

        return 1;

 err:
        cleanup_device_address(devaddr);
        free(value);

        return 0;
}

static int parse_fs_device(xmlNode *dnode, struct virt_device **vdevs)
{
        struct virt_device *vdev = NULL;
        struct disk_device *ddev = NULL;
        xmlNode *child = NULL;

        vdev = calloc(1, sizeof(*vdev));
        if (vdev == NULL)
                goto err;

        ddev = (&vdev->dev.disk);

        ddev->type = get_attr_value(dnode, "type");
        if (ddev->type == NULL) {
                CU_DEBUG("No type");
                goto err;
        }

        ddev->access_mode = get_attr_value(dnode, "accessmode");

        for (child = dnode->children; child != NULL; child = child->next) {
                if (XSTREQ(child->name, "source")) {
                        ddev->source = get_attr_value(child, "dir");
                        if (ddev->source == NULL) {
                                CU_DEBUG("No source dir");
                                goto err;
                        }
                } else if (XSTREQ(child->name, "target")) {
                        ddev->virtual_dev = get_attr_value(child, "dir");
                        if (ddev->virtual_dev == NULL) {
                                CU_DEBUG("No target dir");
                                goto err;
                        }
                } else if (XSTREQ(child->name, "driver")) {
                       ddev->driver_type = get_attr_value(child, "type");
                } else if (XSTREQ(child->name, "address")) {
                        parse_device_address(child, &ddev->address);
                }
        }

        if ((ddev->source == NULL) || (ddev->virtual_dev == NULL)) {
                CU_DEBUG("S: %s D: %s", ddev->source, ddev->virtual_dev);
                goto err;
        }

        ddev->disk_type = DISK_FS;

        vdev->type = CIM_RES_TYPE_DISK;
        vdev->id = strdup(ddev->virtual_dev);

        *vdevs = vdev;

        return 1;

 err:
        CU_DEBUG("Error parsing fs");
        cleanup_disk_device(ddev);
        free(vdev);

        return 0;
}

static int parse_block_device(xmlNode *dnode, struct virt_device **vdevs)
{
        struct virt_device *vdev = NULL;
        struct disk_device *ddev = NULL;
        xmlNode * child = NULL;

        vdev = calloc(1, sizeof(*vdev));
        if (vdev == NULL)
                goto err;

        ddev = &(vdev->dev.disk);

        ddev->type = get_attr_value(dnode, "type");
        if (ddev->type == NULL)
                goto err;

        ddev->device = get_attr_value(dnode, "device");
        if (ddev->device == NULL)
                goto err;

        ddev->rawio = get_attr_value(dnode, "rawio");

        ddev->sgio = get_attr_value(dnode, "sgio");

        for (child = dnode->children; child != NULL; child = child->next) {
                if (XSTREQ(child->name, "driver")) {
                        ddev->driver = get_attr_value(child, "name");
                        if (ddev->driver == NULL)
                                goto err;
                        ddev->driver_type = get_attr_value(child, "type");
                        ddev->cache = get_attr_value(child, "cache");
                } else if (XSTREQ(child->name, "source")) {
                        ddev->source = get_attr_value(child, "file");
                        if (ddev->source) {
                                ddev->disk_type = DISK_FILE;
                                continue;
                        }
                        ddev->source = get_attr_value(child, "dev");
                        if (ddev->source) {
                                ddev->disk_type = DISK_PHY;
                                continue;
                        }
                        goto err;
                } else if (XSTREQ(child->name, "target")) {
                        ddev->virtual_dev = get_attr_value(child, "dev");
                        if (ddev->virtual_dev == NULL)
                                goto err;
                        ddev->bus_type = get_attr_value(child, "bus");
                } else if (XSTREQ(child->name, "readonly")) {
                        ddev->readonly = true;
                } else if (XSTREQ(child->name, "shareable")) {
                        ddev->shareable = true;
                } else if (XSTREQ(child->name, "address")) {
                        parse_device_address(child, &ddev->address);
                }
        }

        /* handle the situation that a cdrom device have no disk in it, no ISO file */
        if ((XSTREQ(ddev->device, "cdrom")) && (ddev->source == NULL)) {
                ddev->source = strdup("");
                ddev->disk_type = DISK_FILE;
        }

        if ((ddev->source == NULL) || (ddev->virtual_dev == NULL))
                goto err;

        vdev->type = CIM_RES_TYPE_DISK;
        vdev->id = strdup(ddev->virtual_dev);

        *vdevs = vdev;

        return 1;

 err:
        cleanup_disk_device(ddev);
        free(vdev);

        return 0;
}

static int parse_disk_device(xmlNode *dnode, struct virt_device **vdevs)
{
        CU_DEBUG("Disk node: %s", dnode->name);

        if (XSTREQ(dnode->name, "disk"))
                return parse_block_device(dnode, vdevs);
        else if (XSTREQ(dnode->name, "filesystem"))
                return parse_fs_device(dnode, vdevs);
        else {
                CU_DEBUG("Unknown disk device: %s", dnode->name);
                return 0;
        }
}

static int parse_vsi_device(xmlNode *dnode, struct net_device *vdevs)
{
        struct vsi_device *vsi_dev = NULL;
        xmlNode * child = NULL;

        vsi_dev = calloc(1, sizeof(*vsi_dev));
        if (vsi_dev == NULL)
                goto err;

        vsi_dev->vsi_type = get_attr_value(dnode, "type");
        if (vsi_dev->vsi_type == NULL)
                goto err;

        for (child = dnode->children; child != NULL; child = child->next) {
                if (XSTREQ(child->name, "parameters")) {
                        vsi_dev->manager_id = get_attr_value(child, 
                                                             "managerid");
                        if (vsi_dev->manager_id == NULL)
                                goto err;

                        vsi_dev->type_id = get_attr_value(child, "typeid");
                        if (vsi_dev->type_id == NULL)
                                goto err;

                        vsi_dev->type_id_version = 
                                get_attr_value(child, "typeidversion");
                        if (vsi_dev->type_id_version == NULL)
                                goto err;

                        vsi_dev->instance_id = get_attr_value(child, 
                                                              "instanceid");
                        vsi_dev->profile_id = get_attr_value(child, 
                                                             "profileid");
                 }
        }

        memcpy(&(vdevs->vsi), vsi_dev, sizeof(*vsi_dev));
        free(vsi_dev);
        return 1;

err:
        cleanup_vsi_device(vsi_dev);
        free(vsi_dev);
        return 0;
}

static int parse_net_device(xmlNode *inode, struct virt_device **vdevs)
{
        struct virt_device *vdev = NULL;
        struct net_device *ndev = NULL;
        xmlNode *child = NULL;

        vdev = calloc(1, sizeof(*vdev));
        if (vdev == NULL)
                goto err;

        ndev = &(vdev->dev.net);

        ndev->type = get_attr_value(inode, "type");
        if (ndev->type == NULL)
                goto err;

        for (child = inode->children; child != NULL; child = child->next) {
                if (XSTREQ(child->name, "mac")) {
                        ndev->mac = get_attr_value(child, "address");
                        if (ndev->mac == NULL)
                                goto err;
                } else if (XSTREQ(child->name, "source")) {
                        ndev->source = get_attr_value(child, "bridge");
                        if (ndev->source != NULL)
                                continue;
                        ndev->source = get_attr_value(child, "network");
                        if (ndev->source != NULL) {
                                int ret = asprintf(&ndev->poolid, 
                                                   "NetworkPool/%s",
                                                   ndev->source);
                                if (ret == -1) {
                                        CU_DEBUG("Failed to get network"
                                                 " poolid");
                                }
                                continue;
                        }
                        ndev->source = get_attr_value(child, "dev");
                        ndev->net_mode = get_attr_value(child, "mode");
                        if ((ndev->source != NULL) && (ndev->net_mode != NULL))
                                continue;
                        goto err;
                } else if (XSTREQ(child->name, "target")) {
                        ndev->device = get_attr_value(child, "dev");
                        if (ndev->device == NULL)
                                goto err;
                } else if (XSTREQ(child->name, "model")) {
                        ndev->model = get_attr_value(child, "type");
                        if (ndev->model == NULL)
                                goto err;
                } else if (XSTREQ(child->name, "filterref")) {
                        ndev->filter_ref = get_attr_value(child, "filter");
                } else if (XSTREQ(child->name, "virtualport")) {
                        parse_vsi_device(child, ndev);
                } else if (XSTREQ(child->name, "address")) {
                        parse_device_address(child, &ndev->address);
#if LIBVIR_VERSION_NUMBER >= 9000
                } else if (XSTREQ(child->name, "bandwidth")) {
                        /* Network QoS bandwidth support */
                        xmlNode *grandchild = NULL;
                        for (grandchild = child->children;
                             grandchild != NULL;
                             grandchild = grandchild->next) {
                                if (XSTREQ(grandchild->name, "inbound")) {
                                          /* Only expose inbound bandwidth */
                                          char *val;

                                          val = get_attr_value(grandchild,
                                                               "average");
                                          if (val != NULL) {
                                                  sscanf(val, "%" PRIu64,
                                                     &ndev->reservation);
                                                  free(val);
                                          } else
                                                  ndev->reservation = 0;

                                          val = get_attr_value(grandchild,
                                                               "peak");
                                          if (val != NULL) {
                                                  sscanf(val, "%" PRIu64,
                                                     &ndev->limit);
                                                  free(val);
                                          } else
                                                  ndev->limit = 0;
                                          break;
                                }
                        }
                }
#endif

        }

        if (ndev->source == NULL)
                CU_DEBUG("No network source defined, leaving blank\n");

        vdev->type = CIM_RES_TYPE_NET;
        vdev->id = strdup(ndev->mac);

        *vdevs = vdev;

        return 1;
  err:
        cleanup_net_device(ndev);
        free(vdev);

        return 0;
}

static int parse_vcpu_device(xmlNode *node, struct virt_device **vdevs)
{
        struct virt_device *list = NULL;
        char *count_str;
        int count;

        count_str = get_node_content(node);
        if (count_str == NULL)
                count = 1; /* Default to 1 VCPU if non specified */
        else if (sscanf(count_str, "%i", &count) != 1)
                count = 1; /* Default to 1 VCPU if garbage */

        free(count_str);

        list = calloc(1, sizeof(*list));
        if (list == NULL)
                goto err;
        
        list->dev.vcpu.quantity = count;

        list->type = CIM_RES_TYPE_PROC;
        list->id = strdup("proc");

        *vdevs = list;

        return 1;
 err:
        free(list);

        return 0;
}

static int parse_emu_device(xmlNode *node, struct virt_device **vdevs)
{
        struct virt_device *vdev = NULL;
        struct emu_device *edev = NULL;

        vdev = calloc(1, sizeof(*vdev));
        if (vdev == NULL)
                goto err;

        edev = &(vdev->dev.emu);

        edev->path = get_node_content(node);
        if (edev->path == NULL)
                goto err;

        vdev->type = CIM_RES_TYPE_EMU;

        *vdevs = vdev;

        return 1;
 err:
        cleanup_emu_device(edev);
        free(vdev);

        return 0;
}

static int parse_mem_device(xmlNode *node, struct virt_device **vdevs)
{
        struct virt_device *vdev = NULL;
        struct mem_device *mdev = NULL;
        char *content = NULL;
        char *tmpval = NULL;
        int ret = 0;

        vdev = calloc(1, sizeof(*vdev));
        if (vdev == NULL)
                goto err;

        mdev = &(vdev->dev.mem);

        content = get_node_content(node);

        if (XSTREQ(node->name, "currentMemory"))
                sscanf(content, "%" PRIu64, &mdev->size);
        else if (XSTREQ(node->name, "memory")) {
                sscanf(content, "%" PRIu64, &mdev->maxsize);
                tmpval = get_attr_value(node, "dumpCore");
                if (tmpval && STREQC(tmpval, "on")) {
                    mdev->dumpCore = MEM_DUMP_CORE_ON;
                } else if (tmpval && STREQC(tmpval, "off")) {
                    mdev->dumpCore = MEM_DUMP_CORE_OFF;
                } else {
                    mdev->dumpCore = MEM_DUMP_CORE_NOT_SET;
                }
        }

        *vdevs = vdev;
        vdev = NULL;
        ret = 1;

 err:
        free(content);
        free(tmpval);
        free(vdev);

        return ret;
}

static char *get_attr_value_default(xmlNode *node, char *attrname,
                                    const char *default_value)
{
        char *ret = get_attr_value(node, attrname);

        if (ret == NULL && default_value != NULL)
                ret = strdup(default_value);

        return ret;
}

static int parse_console_device(xmlNode *node, struct virt_device **vdevs)
{
        struct virt_device *vdev = NULL;
        struct console_device *cdev = NULL;
        char *source_type_str = NULL;
        char *target_port_ID = NULL;

        xmlNode *child = NULL;

        vdev = calloc(1, sizeof(*vdev));
        if (vdev == NULL)
                goto err;

        cdev = &(vdev->dev.console);

        source_type_str = get_attr_value(node, "type");
        if (source_type_str == NULL)
                goto err;
        CU_DEBUG("console device type = %s", source_type_str ? : "NULL");

        cdev->source_type = chardev_source_type_StrToID(source_type_str);
        if (cdev->source_type == CIM_CHARDEV_SOURCE_TYPE_UNKNOWN)
                goto err;

        CU_DEBUG("console device type ID = %d", cdev->source_type);

        for (child = node->children; child != NULL; child = child->next) {
                if (XSTREQ(child->name, "target") && target_port_ID == NULL) {
                        cdev->target_type = get_attr_value(child, "type");
                        CU_DEBUG("Console device target type = '%s'",
                                 cdev->target_type ? : "NULL");
                        target_port_ID = get_attr_value(child, "port");
                        if (target_port_ID == NULL)
                                goto err;
                }

                if (XSTREQ(child->name, "source")) {
                        switch (cdev->source_type)
                        {
                        case CIM_CHARDEV_SOURCE_TYPE_PTY:
                                cdev->source_dev.pty.path =
                                        get_attr_value(child, "path");
                                break;
                        case CIM_CHARDEV_SOURCE_TYPE_DEV:
                                cdev->source_dev.dev.path =
                                        get_attr_value(child, "path");
                                break;
                        case CIM_CHARDEV_SOURCE_TYPE_FILE:
                                cdev->source_dev.file.path =
                                        get_attr_value(child, "path");
                                break;
                        case CIM_CHARDEV_SOURCE_TYPE_PIPE:
                                cdev->source_dev.pipe.path =
                                        get_attr_value(child, "path");
                                break;
                        case CIM_CHARDEV_SOURCE_TYPE_UNIXSOCK:
                                cdev->source_dev.unixsock.mode =
                                        get_attr_value(child, "mode");
                                cdev->source_dev.unixsock.path =
                                        get_attr_value(child, "path");
                                break;
                        case CIM_CHARDEV_SOURCE_TYPE_UDP:
                        {
                                char *udp_source_mode = get_attr_value(child,
                                                                       "mode");
                                if (udp_source_mode == NULL)
                                        goto err;
                                if (STREQC(udp_source_mode, "bind")) {
                                        cdev->source_dev.udp.bind_host =
                                                get_attr_value(child, "host");
                                        cdev->source_dev.udp.bind_service =
                                                get_attr_value(child, "service");
                                } else if (STREQC(udp_source_mode, "connect")) {
                                        cdev->source_dev.udp.connect_host =
                                                get_attr_value(child, "host");
                                        cdev->source_dev.udp.connect_service =
                                                get_attr_value(child, "service");
                                } else {
                                        CU_DEBUG("unknown udp mode: %s",
                                                 udp_source_mode);
                                        free(udp_source_mode);
                                        goto err;
                                }
                                free(udp_source_mode);
                                break;
                        }
                        case CIM_CHARDEV_SOURCE_TYPE_TCP:
                                cdev->source_dev.tcp.mode =
                                        get_attr_value(child, "mode");
                                cdev->source_dev.tcp.host =
                                        get_attr_value(child, "host");
                                cdev->source_dev.tcp.service =
                                        get_attr_value(child, "service");
                                break;

                        default:
                                /* Nothing to do for :
                                   CIM_CHARDEV_SOURCE_TYPE_STDIO
                                   CIM_CHARDEV_SOURCE_TYPE_NULL
                                   CIM_CHARDEV_SOURCE_TYPE_VC
                                   CIM_CHARDEV_SOURCE_TYPE_SPICEVMC
                                */
                                break;
                        }
                }
                if ((cdev->source_type == CIM_CHARDEV_SOURCE_TYPE_TCP)
                     && XSTREQ(child->name, "protocol")) {
                        cdev->source_dev.tcp.protocol =
                                get_attr_value(child, "type");
                }
        }

        vdev->type = CIM_RES_TYPE_CONSOLE;

        if (asprintf(&vdev->id, "charconsole:%s", target_port_ID) == -1) {
                CU_DEBUG("Failed to create charconsole id string");
                goto err;
        }

        *vdevs = vdev;
        free(source_type_str);
        free(target_port_ID);

        return 1;

 err:
        free(source_type_str);
        free(target_port_ID);
        cleanup_console_device(cdev);
        free(vdev);

        return 0;
}

static int parse_graphics_device(xmlNode *node, struct virt_device **vdevs)
{
        struct virt_device *vdev = NULL;
        struct graphics_device *gdev = NULL;
        xmlNode *child = NULL;
        int ret;

        vdev = calloc(1, sizeof(*vdev));
        if (vdev == NULL)
                goto err;

        gdev = &(vdev->dev.graphics);

        gdev->type = get_attr_value(node, "type");
        if (gdev->type == NULL)
                goto err;

        CU_DEBUG("graphics device type = %s", gdev->type);

        if (STREQC(gdev->type, "vnc")) {
                gdev->dev.vnc.port = get_attr_value_default(node, "port",
                                                            "-1");
                gdev->dev.vnc.host = get_attr_value_default(node, "listen",
                                                            "127.0.0.1");
                gdev->dev.vnc.keymap = get_attr_value(node, "keymap");
                gdev->dev.vnc.passwd = get_attr_value(node, "passwd");

                if (gdev->dev.vnc.port == NULL || gdev->dev.vnc.host == NULL) {
                        CU_DEBUG("Error vnc port '%p' host '%p'",
                                 gdev->dev.vnc.port, gdev->dev.vnc.host);
                        goto err;
                }
        }
        else if (STREQC(gdev->type, "sdl")) {
                gdev->dev.sdl.display = get_attr_value(node, "display");
                gdev->dev.sdl.xauth = get_attr_value(node, "xauth");
                gdev->dev.sdl.fullscreen = get_attr_value(node, "fullscreen");
        }
        else if (STREQC(gdev->type, "pty")) {
                if (node->name == NULL)
                        goto err;

                /* Change type to serial, console, etc. It will be converted 
                 * back in xmlgen.c */
                free(gdev->type);
                gdev->type = strdup((char *)node->name);

                for (child = node->children; child != NULL; 
                        child = child->next) {
                        if (XSTREQ(child->name, "source")) 
                                gdev->dev.vnc.host = get_attr_value(child, "path");
                        else if (XSTREQ(child->name, "target")) {
                                gdev->dev.vnc.port =
                                        get_attr_value(child, "port");
                        }
                }
        }
        else {
                CU_DEBUG("Unknown graphics type %s", gdev->type);
                goto err;
        }

        vdev->type = CIM_RES_TYPE_GRAPHICS;
        
        if (STREQC(gdev->type, "vnc")) 
                ret = asprintf(&vdev->id, "%s", gdev->type);
        else
                ret = asprintf(&vdev->id, "%s:%s", gdev->type, gdev->dev.vnc.port);

        if (ret == -1) {
                CU_DEBUG("Failed to create graphics is string");
                goto err;
        }

        *vdevs = vdev;

        return 1;
 err:
        cleanup_graphics_device(gdev);
        free(vdev);

        return 0;
}

static int parse_input_device(xmlNode *node, struct virt_device **vdevs)
{
        struct virt_device *vdev = NULL;
        struct input_device *idev = NULL;
        int ret;

        vdev = calloc(1, sizeof(*vdev));
        if (vdev == NULL)
                goto err;

        idev = &(vdev->dev.input);

        idev->type = get_attr_value(node, "type");
        idev->bus = get_attr_value(node, "bus");

        if ((idev->type == NULL) || (idev->bus == NULL))
                goto err;

        vdev->type = CIM_RES_TYPE_INPUT;

        ret = asprintf(&vdev->id, "%s:%s", idev->type, idev->bus);
        if (ret == -1) {
                CU_DEBUG("Failed to create input id string");
                goto err;
        }

        *vdevs = vdev;

        return 1;
 err:
        cleanup_input_device(idev);
        free(vdev);

        return 0;
}

static int parse_controller_device(xmlNode *cnode, struct virt_device **vdevs)
{
        struct virt_device *vdev = NULL;
        struct controller_device *cdev = NULL;
        char *type_str = NULL;
        xmlNode *child = NULL;
        char *index = NULL;
        int ret;

        vdev = calloc(1, sizeof(*vdev));
        if (vdev == NULL)
                goto err;

        cdev = &(vdev->dev.controller);

        type_str = get_attr_value(cnode, "type");
        if (type_str == NULL) {
                CU_DEBUG("No type");
                goto err;
        }
        CU_DEBUG("controller device type = %s", type_str);
        cdev->type = controller_protocol_type_StrToID(type_str);
        if (cdev->type == CIM_CONTROLLER_PROTOCOL_TYPE_UNKNOWN) {
                CU_DEBUG("Unknown controller protocol type (%d)", cdev->type);
                goto err;
        }

        index = get_attr_value(cnode, "index");
        if (index != NULL) {
                sscanf(index, "%" PRIu64, &cdev->index);
                free(index);
        } else {
                CU_DEBUG("No index");
                goto err;
        }

        cdev->model = get_attr_value(cnode, "model");
        cdev->ports = get_attr_value(cnode, "ports");
        cdev->vectors = get_attr_value(cnode, "vectors");

        for (child = cnode->children; child != NULL; child = child->next) {
                if (XSTREQ(child->name, "address")) {
                        parse_device_address(child, &cdev->address);
                } else if (XSTREQ(child->name, "driver")) {
                        cdev->queues = get_attr_value(child, "queues");
                }
        }
        vdev->type = CIM_RES_TYPE_CONTROLLER;

        ret = asprintf(&vdev->id, "controller:%s:%" PRIu64,
                       type_str, cdev->index);
        if (ret == -1) {
                CU_DEBUG("Failed to create controller id string");
                goto err;
        }
        CU_DEBUG("Controller id is %s", vdev->id);
        free(type_str);

        *vdevs = vdev;

        return 1;
 err:
        free(type_str);
        cleanup_controller_device(cdev);
        free(vdev);

        return 0;
}

static bool resize_devlist(struct virt_device **list, int newsize)
{
        struct virt_device *_list;

        _list = realloc(*list, newsize * sizeof(struct virt_device));
        if (_list == NULL)
                return false;

        *list = _list;

        return true;
}


static int do_parse(xmlNodeSet *nsv, dev_parse_func_t do_real_parse,
                    struct virt_device **l)
{
        int devidx;
        int lstidx = 0;
        int count = 0;
        struct virt_device *list = NULL;
        xmlNode **dev_nodes = NULL;

        if (nsv == NULL)
                goto out;
        count = nsv->nodeNr;

        dev_nodes = nsv->nodeTab;

        if (count <= 0)
                goto out;

        /* walk thru the array, do real parsing on each node */
        for (devidx = 0; devidx < count; devidx++) {
                struct virt_device *tmp_list = NULL;
                int devices = 0;

                devices = do_real_parse(dev_nodes[devidx], &tmp_list);
                if (devices <= 0)
                        continue;

                if (!resize_devlist(&list, lstidx + devices)) {
                        /* Skip these devices and try again for the
                         * next cycle, which will probably fail, but
                         * what else can you do?
                         */
                        goto end;
                }

                memcpy(&list[lstidx], tmp_list, devices * sizeof(*tmp_list));
                lstidx += devices;
        end:
                free(tmp_list);
        }

  out:
        if (list) {
                free(*l);
                *l = list;
        }
        return lstidx;
}

/* Dummy function to suppress error message from libxml2 */
static void swallow_err_msg(void *ctx, const char *msg, ...)
{
        /* do nothing, just swallow the message. */
}

static int parse_devices(const char *xml, struct virt_device **_list, int type)
{
        int len = 0;
        int count = 0;
        dev_parse_func_t func = NULL;

        xmlDoc *xmldoc;
        xmlXPathContext *xpathCtx;
        xmlXPathObject *xpathObj;
        xmlChar *xpathstr;

        CU_DEBUG("In parse_devices - type is %d", type);

        switch (type) {
        case CIM_RES_TYPE_NET:
                xpathstr = NET_XPATH;
                func = &parse_net_device;
                break;

        case CIM_RES_TYPE_DISK:
                xpathstr = DISK_XPATH;
                func = &parse_disk_device;
                break;

        case CIM_RES_TYPE_PROC:
                xpathstr = VCPU_XPATH;
                func = &parse_vcpu_device;
                break;

        case CIM_RES_TYPE_EMU:
                xpathstr = EMU_XPATH;
                func = &parse_emu_device;
                break;

        case CIM_RES_TYPE_MEM:
                xpathstr = MEM_XPATH;
                func = &parse_mem_device;
                break;

        case CIM_RES_TYPE_GRAPHICS:
                xpathstr = GRAPHICS_XPATH;
                func = &parse_graphics_device;
                break;

        case CIM_RES_TYPE_CONSOLE:
                xpathstr = CONSOLE_XPATH;
                func = &parse_console_device;
                break;

        case CIM_RES_TYPE_INPUT:
                xpathstr = INPUT_XPATH;
                func = &parse_input_device;
                break;

        case CIM_RES_TYPE_CONTROLLER:
                xpathstr = CONTROLLER_XPATH;
                func = &parse_controller_device;
                break;

        default:
                CU_DEBUG("Unrecognized device type. Returning.");
                goto err1;
        };

        len = strlen(xml) + 1;

        xmlSetGenericErrorFunc(NULL, swallow_err_msg);
        if ((xmldoc = xmlParseMemory(xml, len)) == NULL)
                goto err1;

        if ((xpathCtx = xmlXPathNewContext(xmldoc)) == NULL)
                goto err2;

        if ((xpathObj = xmlXPathEvalExpression(xpathstr, xpathCtx))
                        == NULL)
                goto err3;

        count = do_parse(xpathObj->nodesetval, func, _list);

        xmlSetGenericErrorFunc(NULL, NULL);
        xmlXPathFreeObject(xpathObj);
  err3:
        xmlXPathFreeContext(xpathCtx);
  err2:
        xmlFreeDoc(xmldoc);
  err1:
        return count;
}

static void duplicate_device_address(struct device_address *to, const struct device_address *from)
{
        int i;

        if (from == NULL || to == NULL || from->ct == 0)
                return;

        to->ct = from->ct;
        to->key = calloc(from->ct, sizeof(char*));
        to->value = calloc(from->ct, sizeof(char*));
        if (to->key == NULL || to->value == NULL)
                goto err;

        for (i = 0; i < from->ct; i++) {
                to->key[i] = strdup(from->key[i]);
                to->value[i] = strdup(from->value[i]);
                if (to->key[i] == NULL || to->value[i] == NULL)
                        goto err;
        }

        return;

 err:
        cleanup_device_address(to);
}

struct virt_device *virt_device_dup(struct virt_device *_dev)
{
        struct virt_device *dev;

        dev = calloc(1, sizeof(*dev));
        if (dev == NULL)
                return NULL;

        dev->type = _dev->type;
        dev->id = strdup(_dev->id);

        if (dev->type == CIM_RES_TYPE_NET) {
                DUP_FIELD(dev, _dev, dev.net.mac);
                DUP_FIELD(dev, _dev, dev.net.type);
                DUP_FIELD(dev, _dev, dev.net.source);
                DUP_FIELD(dev, _dev, dev.net.model);
                DUP_FIELD(dev, _dev, dev.net.poolid);
                DUP_FIELD(dev, _dev, dev.net.device);
                DUP_FIELD(dev, _dev, dev.net.net_mode);
                DUP_FIELD(dev, _dev, dev.net.filter_ref);
                DUP_FIELD(dev, _dev, dev.net.vsi.vsi_type);
                DUP_FIELD(dev, _dev, dev.net.vsi.manager_id);
                DUP_FIELD(dev, _dev, dev.net.vsi.type_id);
                DUP_FIELD(dev, _dev, dev.net.vsi.type_id_version);
                DUP_FIELD(dev, _dev, dev.net.vsi.instance_id);
                DUP_FIELD(dev, _dev, dev.net.vsi.filter_ref);
                DUP_FIELD(dev, _dev, dev.net.vsi.profile_id);
                dev->dev.net.reservation = _dev->dev.net.reservation;
                dev->dev.net.limit = _dev->dev.net.limit;
                duplicate_device_address(&dev->dev.net.address, &_dev->dev.net.address);
        } else if (dev->type == CIM_RES_TYPE_DISK) {
                DUP_FIELD(dev, _dev, dev.disk.type);
                DUP_FIELD(dev, _dev, dev.disk.device);
                DUP_FIELD(dev, _dev, dev.disk.driver);
                DUP_FIELD(dev, _dev, dev.disk.driver_type);
                DUP_FIELD(dev, _dev, dev.disk.rawio);
                DUP_FIELD(dev, _dev, dev.disk.sgio);
                DUP_FIELD(dev, _dev, dev.disk.cache);
                DUP_FIELD(dev, _dev, dev.disk.source);
                DUP_FIELD(dev, _dev, dev.disk.virtual_dev);
                DUP_FIELD(dev, _dev, dev.disk.bus_type);
                DUP_FIELD(dev, _dev, dev.disk.access_mode);
                dev->dev.disk.disk_type = _dev->dev.disk.disk_type;
                dev->dev.disk.readonly = _dev->dev.disk.readonly;
                dev->dev.disk.shareable = _dev->dev.disk.shareable;
                duplicate_device_address(&dev->dev.disk.address, &_dev->dev.disk.address);
        } else if (dev->type == CIM_RES_TYPE_MEM) {
                dev->dev.mem.size = _dev->dev.mem.size;
                dev->dev.mem.maxsize = _dev->dev.mem.maxsize;
                dev->dev.mem.dumpCore = _dev->dev.mem.dumpCore;
        } else if (dev->type == CIM_RES_TYPE_PROC) {
                dev->dev.vcpu.quantity = _dev->dev.vcpu.quantity;
        } else if (dev->type == CIM_RES_TYPE_EMU) {
                DUP_FIELD(dev, _dev, dev.emu.path);
        } else if (dev->type == CIM_RES_TYPE_GRAPHICS) {
                DUP_FIELD(dev, _dev, dev.graphics.type);
                DUP_FIELD(dev, _dev, dev.graphics.dev.vnc.host);
                DUP_FIELD(dev, _dev, dev.graphics.dev.vnc.port);
                DUP_FIELD(dev, _dev, dev.graphics.dev.vnc.keymap);
                DUP_FIELD(dev, _dev, dev.graphics.dev.vnc.passwd);
        } else if (dev->type == CIM_RES_TYPE_INPUT) {
                DUP_FIELD(dev, _dev, dev.input.type);
                DUP_FIELD(dev, _dev, dev.input.bus);
        } else if (dev->type == CIM_RES_TYPE_CONSOLE) {
                console_device_dup(&dev->dev.console,
                                   &_dev->dev.console);
        } else if (dev->type == CIM_RES_TYPE_CONTROLLER) {
                dev->dev.controller.type = _dev->dev.controller.type;
                dev->dev.controller.index = _dev->dev.controller.index;
                DUP_FIELD(dev, _dev, dev.controller.model);
                DUP_FIELD(dev, _dev, dev.controller.ports);
                DUP_FIELD(dev, _dev, dev.controller.vectors);
                DUP_FIELD(dev, _dev, dev.controller.queues);
                duplicate_device_address(&dev->dev.controller.address,
                                         &_dev->dev.controller.address);
        }

        return dev;
}

static int _get_mem_device(const char *xml, struct virt_device **list)
{
        struct virt_device *mdevs = NULL;
        struct virt_device *mdev = NULL;
        int ret;
        bool mem_dump_core_set = false;

        ret = parse_devices(xml, &mdevs, CIM_RES_TYPE_MEM);
        if (ret <= 0)
                return ret;

        mdev = calloc(1, sizeof(*mdev));
        if (mdev == NULL)
                return 0;

        /* We could get one or two memory devices back, depending on
         * if there is a currentMemory tag or not.  Coalesce these
         * into a single device to return
         */

        if (ret == 2) {
                mdev->dev.mem.size = MAX(mdevs[0].dev.mem.size,
                                         mdevs[1].dev.mem.size);
                mdev->dev.mem.maxsize = MAX(mdevs[0].dev.mem.maxsize,
                                            mdevs[1].dev.mem.maxsize);
                /* libvirt dumpCore tag always belong to memory xml node, but
                 * here we may have two mdev for memory node and currentMemory
                 * node. So pick up one value.
                 */
                if (mdevs[0].dev.mem.dumpCore != MEM_DUMP_CORE_NOT_SET) {
                        mdev->dev.mem.dumpCore = mdevs[0].dev.mem.dumpCore;
                        mem_dump_core_set = true;
                } else if (mdevs[1].dev.mem.dumpCore !=
                           MEM_DUMP_CORE_NOT_SET) {
                        if (mem_dump_core_set) {
                                CU_DEBUG("WARN: libvirt set memory core dump in"
                                         "two nodes!");
                        }
                        mdev->dev.mem.dumpCore = mdevs[1].dev.mem.dumpCore;
                }
        } else {
                mdev->dev.mem.size = MAX(mdevs[0].dev.mem.size,
                                         mdevs[0].dev.mem.maxsize);
                mdev->dev.mem.maxsize = mdev->dev.mem.size;
                mdev->dev.mem.dumpCore = mdevs[0].dev.mem.dumpCore;
        }

        mdev->type = CIM_RES_TYPE_MEM;
        mdev->id = strdup("mem");
        *list = mdev;

        cleanup_virt_devices(&mdevs, ret);

        return 1;
}

static int _get_proc_device(const char *xml, struct virt_device **list)
{
        struct virt_device *proc_devs = NULL;
        struct virt_device *proc_dev = NULL;
        int ret;

        ret = parse_devices(xml, &proc_devs, CIM_RES_TYPE_PROC);
        if (ret <= 0)
                return ret;

        proc_dev = calloc(1, sizeof(*proc_dev));
        if (proc_dev == NULL)
                return 0;

        proc_dev->type = CIM_RES_TYPE_PROC;
        proc_dev->id = strdup("proc");
        proc_dev->dev.vcpu.quantity = proc_devs[0].dev.vcpu.quantity;
        *list = proc_dev;

        cleanup_virt_devices(&proc_devs, ret);

        return 1;
};

int get_devices(virDomainPtr dom, struct virt_device **list, int type,
                                                    unsigned int flags)
{
        char *xml;
        int ret;

        xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_SECURE | flags);
        if (xml == NULL)
                return 0;

        if (type == CIM_RES_TYPE_MEM)
                ret = _get_mem_device(xml, list);
        else if (type == CIM_RES_TYPE_PROC)
                ret = _get_proc_device(xml, list);
        else
                ret = parse_devices(xml, list, type);

        free(xml);

        return ret;
}

char *get_fq_devid(char *host, char *_devid)
{
        char *devid;

        if (asprintf(&devid, "%s/%s", host, _devid) == -1)
                return NULL;
        else
                return devid;
}

int parse_fq_devid(const char *devid, char **host, char **device)
{
        int ret;

        ret = sscanf(devid, "%m[^/]/%m[^\n]", host, device);
        if (ret != 2) {
                free(*host);
                free(*device);

                *host = NULL;
                *device = NULL;

                return 0;
        }

        return 1;
}

static void cleanup_bootlist(char **blist, unsigned blist_ct)
{
        while (blist_ct > 0) {
                free(blist[--blist_ct]);
        }
        free(blist);
}

static int parse_os(struct domain *dominfo, xmlNode *os)
{
        xmlNode *child;
        char **blist = NULL;
        unsigned bl_size = 0;
        char *arch = NULL;
        char *machine = NULL;
        char *kernel = NULL;
        char *initrd = NULL;
        char *cmdline = NULL;
        char *loader = NULL;
        char *boot = NULL;
        char *init = NULL;

        for (child = os->children; child != NULL; child = child->next) {
                if (XSTREQ(child->name, "type")) {
                        STRPROP(dominfo, os_info.pv.type, child);
                        if (arch == NULL)
                            arch = get_attr_value(child, "arch");
                        if (machine == NULL)
                            machine = get_attr_value(child, "machine");
                } else if (XSTREQ(child->name, "kernel") && kernel == NULL)
                        kernel = get_node_content(child);
                else if (XSTREQ(child->name, "initrd") && initrd == NULL)
                        initrd = get_node_content(child);
                else if (XSTREQ(child->name, "cmdline") && cmdline == NULL)
                        cmdline = get_node_content(child);
                else if (XSTREQ(child->name, "loader") && loader == NULL)
                        loader = get_node_content(child);
                else if (XSTREQ(child->name, "boot") && boot == NULL) {
                        char **tmp_list = NULL;

                        tmp_list = (char **)realloc(blist,
                                                    (bl_size + 1) *
                                                    sizeof(char *));
                        if (tmp_list == NULL) {
                                // Nothing you can do. Just go on.
                                CU_DEBUG("Could not alloc space for "
                                         "boot device");
                                continue;
                        }
                        blist = tmp_list;

                        blist[bl_size] = get_attr_value(child, "dev");
                        bl_size++;
                } else if (XSTREQ(child->name, "init") && init == NULL)
                        init = get_node_content(child);
        }

        if ((STREQC(dominfo->os_info.fv.type, "hvm")) &&
            (STREQC(dominfo->typestr, "xen")))
                dominfo->type = DOMAIN_XENFV;
        else if (STREQC(dominfo->typestr, "kvm"))
                dominfo->type = DOMAIN_KVM;
        else if (STREQC(dominfo->typestr, "qemu"))
                dominfo->type = DOMAIN_QEMU;
        else if (STREQC(dominfo->typestr, "lxc"))
                dominfo->type = DOMAIN_LXC;
        else if (STREQC(dominfo->os_info.pv.type, "linux"))
                dominfo->type = DOMAIN_XENPV;
        else
                dominfo->type = -1;

        switch (dominfo->type) {
        case DOMAIN_XENFV:
        case DOMAIN_KVM:
        case DOMAIN_QEMU:
                dominfo->os_info.fv.loader = loader;
                dominfo->os_info.fv.arch = arch;
                dominfo->os_info.fv.machine = machine;
                dominfo->os_info.fv.bootlist_ct = bl_size;
                dominfo->os_info.fv.bootlist = blist;
                loader = NULL;
                arch = NULL;
                machine = NULL;
                blist = NULL;
                bl_size = 0;
                break;
        case DOMAIN_XENPV:
                dominfo->os_info.pv.kernel = kernel;
                dominfo->os_info.pv.initrd = initrd;
                dominfo->os_info.pv.cmdline = cmdline;
                kernel = NULL;
                initrd = NULL;
                cmdline = NULL;
                break;
        case DOMAIN_LXC:
                dominfo->os_info.lxc.init = init;
                init = NULL;
                break;
        default:
                break;
        }

        free(arch);
        free(machine);
        free(kernel);
        free(initrd);
        free(cmdline);
        free(loader);
        free(boot);
        free(init);
        cleanup_bootlist(blist, bl_size);
        return 1;
}

static int parse_features(struct domain *dominfo, xmlNode *features)
{
        xmlNode *child;

        for (child = features->children; child != NULL; child = child->next) {
                if (XSTREQ(child->name, "acpi"))
                        dominfo->acpi = true;
                else if (XSTREQ(child->name, "apic"))
                        dominfo->apic = true;
                else if (XSTREQ(child->name, "pae"))
                        dominfo->pae = true;
        }

        return 1;
}

static void set_action(uint16_t *val, xmlNode *child)
{
        char *action = (char *)xmlNodeGetContent(child);

        if (action == NULL)
                *val = CIM_VSSD_RECOVERY_NONE;
        else if (STREQ(action, "destroy"))
                *val = CIM_VSSD_RECOVERY_NONE;
        else if (STREQ(action, "preserve"))
                *val = CIM_VSSD_RECOVERY_PRESERVE;
        else if (STREQ(action, "restart"))
                *val = CIM_VSSD_RECOVERY_RESTART;
        else
                *val = CIM_VSSD_RECOVERY_NONE;

        xmlFree(action);
}

static int parse_domain(xmlNodeSet *nsv, struct domain *dominfo)
{
        xmlNode **nodes = nsv->nodeTab;
        xmlNode *child;

        dominfo->typestr = get_attr_value(nodes[0], "type");

        for (child = nodes[0]->children; child != NULL; child = child->next) {
                if (XSTREQ(child->name, "name"))
                        STRPROP(dominfo, name, child);
                else if (XSTREQ(child->name, "uuid"))
                        STRPROP(dominfo, uuid, child);
                else if (XSTREQ(child->name, "bootloader"))
                        STRPROP(dominfo, bootloader, child);
                else if (XSTREQ(child->name, "bootloader_args"))
                        STRPROP(dominfo, bootloader_args, child);
                else if (XSTREQ(child->name, "os"))
                        parse_os(dominfo, child);
                else if (XSTREQ(child->name, "on_poweroff"))
                        set_action(&dominfo->on_poweroff, child);
                else if (XSTREQ(child->name, "on_reboot"))
                        set_action(&dominfo->on_reboot, child);
                else if (XSTREQ(child->name, "on_crash"))
                        set_action(&dominfo->on_crash, child);
                else if (XSTREQ(child->name, "clock"))
                        dominfo->clock = get_attr_value(child, "offset");
                else if (XSTREQ(child->name, "features"))
                        parse_features(dominfo, child);
        }

        return 1;
}

static int _get_dominfo(const char *xml, struct domain *dominfo)
{
        int len;
        int ret = 0;

        xmlDoc *xmldoc;
        xmlXPathContext *xpathctx;
        xmlXPathObject *xpathobj;
        const xmlChar *xpathstr = (xmlChar *)"/domain";

        len = strlen(xml) + 1;

        if ((xmldoc = xmlParseMemory(xml, len)) == NULL)
                goto err1;

        if ((xpathctx = xmlXPathNewContext(xmldoc)) == NULL)
                goto err2;

        if ((xpathobj = xmlXPathEvalExpression(xpathstr, xpathctx)) == NULL)
                goto err3;

        ret = parse_domain(xpathobj->nodesetval, dominfo);

        xmlXPathFreeObject(xpathobj);
 err3:
        xmlXPathFreeContext(xpathctx);
 err2:
        xmlFreeDoc(xmldoc);
 err1:
        return ret;
}

int get_dominfo_from_xml(const char *xml, struct domain **dominfo)
{
        int ret;

        CU_DEBUG("In get_dominfo_from_xml");
        *dominfo = calloc(1, sizeof(**dominfo));
        if (*dominfo == NULL)
                return 0;

        ret = _get_dominfo(xml, *dominfo);
        if (ret == 0)
                goto err;

        parse_devices(xml, &(*dominfo)->dev_emu, CIM_RES_TYPE_EMU);
        (*dominfo)->dev_graphics_ct = parse_devices(xml, 
                                                    &(*dominfo)->dev_graphics, 
                                                    CIM_RES_TYPE_GRAPHICS);
        (*dominfo)->dev_console_ct = parse_devices(xml,
                                                   &(*dominfo)->dev_console,
                                                   CIM_RES_TYPE_CONSOLE);
        (*dominfo)->dev_input_ct = parse_devices(xml, 
                                                 &(*dominfo)->dev_input, 
                                                 CIM_RES_TYPE_INPUT);
        (*dominfo)->dev_mem_ct = _get_mem_device(xml, &(*dominfo)->dev_mem);
        (*dominfo)->dev_net_ct = parse_devices(xml,
                                               &(*dominfo)->dev_net,
                                               CIM_RES_TYPE_NET);
        (*dominfo)->dev_disk_ct = parse_devices(xml,
                                                &(*dominfo)->dev_disk,
                                                CIM_RES_TYPE_DISK);
        (*dominfo)->dev_vcpu_ct = parse_devices(xml,
                                                &(*dominfo)->dev_vcpu,
                                                CIM_RES_TYPE_PROC);
        (*dominfo)->dev_controller_ct = parse_devices(xml,
                                                      &(*dominfo)->dev_controller,
                                                      CIM_RES_TYPE_CONTROLLER);

        return ret;

 err:
        free(*dominfo);
        *dominfo = NULL;

        return 0;
}

int get_dominfo(virDomainPtr dom, struct domain **dominfo)
{
        char *xml;
        int ret = 0;
        int start;
        int flags = VIR_DOMAIN_XML_INACTIVE;

        if (!is_read_only())
            flags |= VIR_DOMAIN_XML_SECURE;

        xml = virDomainGetXMLDesc(dom, flags);

        if (xml == NULL) {
                CU_DEBUG("Failed to get dom xml with libvirt API.");
                return 0;
        }

        if (get_dominfo_from_xml(xml, dominfo) == 0) {
                CU_DEBUG("Failed to translate xml into struct domain");
                goto out;
        }
        if (virDomainGetAutostart(dom,  &start) !=  0) {
                CU_DEBUG("Failed to get dom autostart with libvirt API.");
                goto out;
        }

        (*dominfo)->autostrt = start;
        ret = 1;

 out:
        free(xml);

        return ret;
}

void cleanup_dominfo(struct domain **dominfo)
{
        struct domain *dom;

        if ((dominfo == NULL) || (*dominfo == NULL))
                return;

        dom = *dominfo;
        free(dom->name);
        free(dom->typestr);
        free(dom->uuid);
        free(dom->bootloader);
        free(dom->bootloader_args);
        free(dom->clock);

        if (dom->type == DOMAIN_XENPV) {
                free(dom->os_info.pv.type);
                free(dom->os_info.pv.kernel);
                free(dom->os_info.pv.initrd);
                free(dom->os_info.pv.cmdline);
        } else if ((dom->type == DOMAIN_XENFV) ||
                   (dom->type == DOMAIN_KVM) || (dom->type == DOMAIN_QEMU)) {
                free(dom->os_info.fv.type);
                free(dom->os_info.fv.loader);
                free(dom->os_info.fv.arch);
                free(dom->os_info.fv.machine);
                cleanup_bootlist(dom->os_info.fv.bootlist,
                                 dom->os_info.fv.bootlist_ct);
        } else if (dom->type == DOMAIN_LXC) {
                free(dom->os_info.lxc.type);
                free(dom->os_info.lxc.init);
        } else {
                CU_DEBUG("Unknown domain type %i", dom->type);
        }

        cleanup_virt_devices(&dom->dev_emu, 1);
        cleanup_virt_devices(&dom->dev_mem, dom->dev_mem_ct);
        cleanup_virt_devices(&dom->dev_net, dom->dev_net_ct);
        cleanup_virt_devices(&dom->dev_disk, dom->dev_disk_ct);
        cleanup_virt_devices(&dom->dev_vcpu, dom->dev_vcpu_ct);
        cleanup_virt_devices(&dom->dev_graphics, dom->dev_graphics_ct);
        cleanup_virt_devices(&dom->dev_input, dom->dev_input_ct);
        cleanup_virt_devices(&dom->dev_console, dom->dev_console_ct);
        cleanup_virt_devices(&dom->dev_controller, dom->dev_controller_ct);

        free(dom);

        *dominfo = NULL;
}

static int _change_device(virDomainPtr dom,
                          struct virt_device *dev,
                          bool attach)
{
        char *xml = NULL;
        int ret = 0;
#if LIBVIR_VERSION_NUMBER >= 4000
        int (*func)(virDomainPtr, const char *);
#else
        int (*func)(virDomainPtr, char *);
#endif

        if (attach)
                func = virDomainAttachDevice;
        else
                func = virDomainDetachDevice;

        xml = device_to_xml(dev);
        if (xml == NULL) {
                CU_DEBUG("Failed to get XML for device `%s'", dev->id);
                goto out;
        }

        if (func(dom, xml) != 0) {
                CU_DEBUG("Failed to dynamically change device:");
                CU_DEBUG("%s", xml);
                goto out;
        }

        ret = 1;
 out:
        free(xml);

        return ret;

}

static int change_disk(virDomainPtr dom,
                       struct virt_device *dev)
{
       
        char *xml = NULL;
        int ret = 0;

        xml = device_to_xml(dev);

        CU_DEBUG("New XML is %s", xml);
        
        if (virDomainAttachDevice(dom, xml) != 0) {
                goto out;
        }

        ret = 1;
 out:
        free(xml);

        return ret;
}

static int change_memory(virDomainPtr dom,
                         struct virt_device *dev)
{
        CU_DEBUG("Changing memory of %s to %llu/%llu",
                 virDomainGetName(dom),
                 dev->dev.mem.size,
                 dev->dev.mem.maxsize);

        if (virDomainSetMemory(dom, dev->dev.mem.size))
                return 0;

        if (virDomainSetMaxMemory(dom, dev->dev.mem.maxsize))
                return 0;

        return 1;
}

static int change_vcpus(virDomainPtr dom, struct virt_device *dev)
{
        int ret;

        /* Change vcpu cgroup cpu_shares */
        if (dev->dev.vcpu.weight > 0) {
                virSchedParameter param;

                strncpy(param.field, "cpu_shares", VIR_DOMAIN_SCHED_FIELD_LENGTH);
                param.type = VIR_DOMAIN_SCHED_FIELD_ULLONG;
                param.value.ul = dev->dev.vcpu.weight;
                
                if (virDomainSetSchedulerParameters(dom, &param, 1) != 0) {
                        CU_DEBUG("Failed to set scheduler params for domain");
                        return 0;
                }

                CU_DEBUG("Changed %s vcpu cgroup cpu_shares to %i",
                         virDomainGetName(dom),
                         dev->dev.vcpu.weight);
        }

        if (dev->dev.vcpu.quantity <= 0) {
                CU_DEBUG("Unable to set VCPU count to %i",
                         dev->dev.vcpu.quantity);
                return 0;
        }

        ret = virDomainSetVcpus(dom, dev->dev.vcpu.quantity);
        if (ret == -1) {
                CU_DEBUG("Failed to set domain vcpus to %i",
                         dev->dev.vcpu.quantity);
                return 0;
        }

        CU_DEBUG("Changed %s vcpus to %i",
                 virDomainGetName(dom),
                 dev->dev.vcpu.quantity);

        return 1;
}

int attach_device(virDomainPtr dom, struct virt_device *dev)
{
        if ((dev->type == CIM_RES_TYPE_NET) ||
            (dev->type == CIM_RES_TYPE_DISK))
                return _change_device(dom, dev, true);

        CU_DEBUG("Unhandled device type %i", dev->type);

        return 0;
}

int detach_device(virDomainPtr dom, struct virt_device *dev)
{
        if ((dev->type == CIM_RES_TYPE_NET) ||
            (dev->type == CIM_RES_TYPE_DISK))
                return _change_device(dom, dev, false);

        CU_DEBUG("Unhandled device type %i", dev->type);

        return 0;
}

int change_device(virDomainPtr dom, struct virt_device *dev)
{
        if (dev->type == CIM_RES_TYPE_MEM)
                return change_memory(dom, dev);
        if (dev->type == CIM_RES_TYPE_PROC)
                return change_vcpus(dom, dev);
        if (dev->type == CIM_RES_TYPE_DISK)
                return change_disk(dom, dev) ;

        CU_DEBUG("Unhandled device type %i", dev->type);

        return 0;
}

int disk_type_from_file(const char *path)
{
        struct stat64 s;

        if (stat64(path, &s) < 0)
                return DISK_UNKNOWN;

        if (S_ISBLK(s.st_mode))
                return DISK_PHY;
        else if (S_ISREG(s.st_mode))
                return DISK_FILE;
        else if (S_ISDIR(s.st_mode))
                return DISK_FS;
        else
                return DISK_UNKNOWN;
}

/*
 * Local Variables:
 * mode: C
 * c-set-style: "K&R"
 * tab-width: 8
 * c-basic-offset: 8
 * indent-tabs-mode: nil
 * End:
 */
