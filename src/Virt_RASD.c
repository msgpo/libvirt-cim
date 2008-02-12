/*
 * Copyright IBM Corp. 2007
 *
 * Authors:
 *  Dan Smith <danms@us.ibm.com>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmpidt.h>
#include <cmpift.h>
#include <cmpimacs.h>

#include <libcmpiutil/libcmpiutil.h>
#include <libcmpiutil/std_instance.h>

#include "device_parsing.h"
#include "misc_util.h"

#include "Virt_RASD.h"
#include "svpc_types.h"

const static CMPIBroker *_BROKER;

static struct virt_device *_find_dev(struct virt_device *list,
                                    int count,
                                    const char *id)
{
        int i;

        for (i = 0; i < count; i++) {
                struct virt_device *dev = &list[i];

                if (STREQ(dev->id, id))
                        return virt_device_dup(dev);
        }

        return NULL;
}

static int list_devs(virConnectPtr conn,
                     const uint16_t type,
                     const char *host,
                     struct virt_device **list)
{
        virDomainPtr dom;

        dom = virDomainLookupByName(conn, host);
        if (dom == NULL)
                return 0;

        return get_devices(dom, list, type);
}

static struct virt_device *find_dev(virConnectPtr conn,
                                    const uint16_t type,
                                    const char *host,
                                    const char *devid)
{
        int count = -1;
        struct virt_device *list = NULL;
        struct virt_device *dev = NULL;

        count = list_devs(conn, type, host, &list);
        if (count > 0) {
                dev = _find_dev(list, count, devid);
                cleanup_virt_devices(&list, count);
        }

        return dev;
}

char *rasd_to_xml(CMPIInstance *rasd)
{
        /* FIXME: Remove this */
        return NULL;
}

static bool proc_get_physical_ref(const CMPIBroker *broker,
                                  uint32_t physnum,
                                  struct inst_list *list)
{
        CMPIObjectPath *op = NULL;
        CMPIStatus s;
        char hostname[255];
        char *devid = NULL;
        CMPIInstance *inst;
        bool result = false;

        if (asprintf(&devid, "%i", physnum) == -1) {
                CU_DEBUG("Failed to create DeviceID string");
                goto out;
        }

        if (gethostname(hostname, sizeof(hostname)) == -1) {
                CU_DEBUG("Hostname overflow");
                goto out;
        }

        op = CMNewObjectPath(broker, "root/cimv2", "Linux_Processor", &s);
        if ((op == NULL) || (s.rc != CMPI_RC_OK)) {
                CU_DEBUG("Failed to get ObjectPath for processor");
                goto out;
        }

        inst = CMNewInstance(broker, op, &s);
        if ((inst == NULL) || (s.rc != CMPI_RC_OK)) {
                CU_DEBUG("Failed to make instance");
                goto out;
        }

        CMSetProperty(inst, "CreationClassName",
                      (CMPIValue *)"Linux_Processor", CMPI_chars);
        CMSetProperty(inst, "SystemName",
                      (CMPIValue *)hostname, CMPI_chars);
        CMSetProperty(inst, "SystemCreationClassName",
                      (CMPIValue *)"Linux_ComputerSystem", CMPI_chars);
        CMSetProperty(inst, "DeviceID",
                      (CMPIValue *)devid, CMPI_chars);

        inst_list_add(list, inst);

        result = true;
 out:
        free(devid);

        return result;
}

static uint32_t proc_set_cpu(const CMPIBroker *broker,
                             virNodeInfoPtr node,
                             virDomainPtr dom,
                             struct virt_device *dev,
                             struct inst_list *list)
{
        virVcpuInfoPtr vinfo = NULL;
        virDomainInfo info;
        uint8_t *cpumaps = NULL;
        int ret;
        int i;
        int vcpu = dev->dev.vcpu.number;
        int maplen = VIR_CPU_MAPLEN(VIR_NODEINFO_MAXCPUS(*node));

        ret = virDomainGetInfo(dom, &info);
        if (ret == -1) {
                CU_DEBUG("Failed to get info for domain `%s'",
                         virDomainGetName(dom));
                goto out;
        }

        if (dev->dev.vcpu.number >= info.nrVirtCpu) {
                CU_DEBUG("VCPU %i higher than max of %i for %s",
                         dev->dev.vcpu.number,
                         info.nrVirtCpu,
                         virDomainGetName(dom));
                goto out;
        }

        vinfo = calloc(info.nrVirtCpu, sizeof(*vinfo));
        if (vinfo == NULL) {
                CU_DEBUG("Failed to allocate memory for %i virVcpuInfo",
                         info.nrVirtCpu);
                goto out;
        }

        cpumaps = calloc(info.nrVirtCpu, maplen);
        if (cpumaps == NULL) {
                CU_DEBUG("Failed to allocate memory for %ix%i maps",
                         info.nrVirtCpu, maplen);
                goto out;
        }

        ret = virDomainGetVcpus(dom, vinfo, info.nrVirtCpu, cpumaps, maplen);
        if (ret < info.nrVirtCpu) {
                CU_DEBUG("Failed to get VCPU info for %s",
                         virDomainGetName(dom));
                goto out;
        }

        for (i = 0; i < VIR_NODEINFO_MAXCPUS(*node); i++) {
                if (VIR_CPU_USABLE(cpumaps, maplen, vcpu, i)) {
                        CU_DEBUG("VCPU %i pinned to physical %i",
                                 vcpu, i);
                        proc_get_physical_ref(broker, i, list);
                } else {
                        CU_DEBUG("VCPU %i not pinned to physical %i",
                                 vcpu, i);
                }
        }
 out:
        free(vinfo);
        free(cpumaps);

        return 0;
}

static CMPIStatus proc_rasd_from_vdev(const CMPIBroker *broker,
                                      struct virt_device *dev,
                                      const char *host,
                                      const CMPIObjectPath *ref,
                                      CMPIInstance *inst)
{
        virConnectPtr conn = NULL;
        virDomainPtr dom = NULL;
        virNodeInfo node;
        CMPIStatus s;
        CMPIArray *array;
        struct inst_list list;

        inst_list_init(&list);

        conn = connect_by_classname(broker, CLASSNAME(ref), &s);
        if (conn == NULL) {
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_FAILED,
                           "Failed to connect for ProcRASD (%s)",
                           CLASSNAME(ref));
                goto out;
        }

        dom = virDomainLookupByName(conn, host);
        if (dom == NULL) {
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_NOT_FOUND,
                           "Unable to get domain for ProcRASD: %s", host);
                goto out;
        }

        if (virNodeGetInfo(virDomainGetConnect(dom), &node) == -1) {
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_FAILED,
                           "Unable to get node info");
                goto out;
        }

        proc_set_cpu(broker, &node, dom, dev, &list);

        if (list.cur > 0) {
                int i;

                array = CMNewArray(broker,
                                   list.cur,
                                   CMPI_instance,
                                   &s);
                for (i = 0; i < list.cur; i++) {
                        CMSetArrayElementAt(array,
                                            i,
                                            (CMPIValue *)&list.list[i],
                                            CMPI_instance);
                }

                CMSetProperty(inst, "HostResource",
                              (CMPIValue *)&array, CMPI_instanceA);
        }

 out:
        inst_list_free(&list);
        virDomainFree(dom);
        virConnectClose(conn);

        return s;
}

static CMPIInstance *rasd_from_vdev(const CMPIBroker *broker,
                                    struct virt_device *dev,
                                    const char *host,
                                    const CMPIObjectPath *ref)
{
        CMPIInstance *inst;
        uint16_t type;
        char *base;
        char *id;

        if (dev->type == VIRT_DEV_DISK) {
                type = CIM_RASD_TYPE_DISK;
                base = "DiskResourceAllocationSettingData";
        } else if (dev->type == VIRT_DEV_NET) {
                type = CIM_RASD_TYPE_NET;
                base = "NetResourceAllocationSettingData";
        } else if (dev->type == VIRT_DEV_VCPU) {
                type = CIM_RASD_TYPE_PROC;
                base = "ProcResourceAllocationSettingData";
        } else if (dev->type == VIRT_DEV_MEM) {
                type = CIM_RASD_TYPE_MEM;
                base = "MemResourceAllocationSettingData";
        } else {
                return NULL;
        }

        inst = get_typed_instance(broker,
                                  CLASSNAME(ref),
                                  base,
                                  NAMESPACE(ref));
        if (inst == NULL)
                return inst;

        id = get_fq_devid((char *)host, dev->id);

        CMSetProperty(inst, "InstanceID",
                      (CMPIValue *)id, CMPI_chars);

        CMSetProperty(inst, "ResourceType",
                      (CMPIValue *)&type, CMPI_uint16);

        if (dev->type == VIRT_DEV_DISK) {
                CMSetProperty(inst,
                              "VirtualDevice",
                              (CMPIValue *)dev->dev.disk.virtual_dev,
                              CMPI_chars);
                CMSetProperty(inst,
                              "Address",
                              (CMPIValue *)dev->dev.disk.source,
                              CMPI_chars);
        } else if (dev->type == VIRT_DEV_NET) {
                CMSetProperty(inst,
                              "NetworkType",
                              (CMPIValue *)dev->dev.disk.type,
                              CMPI_chars);
        } else if (dev->type == VIRT_DEV_MEM) {
                const char *units = "MegaBytes";

                CMSetProperty(inst, "AllocationUnits",
                              (CMPIValue *)units, CMPI_chars);
                CMSetProperty(inst, "VirtualQuantity",
                              (CMPIValue *)&dev->dev.mem.size, CMPI_uint64);
                CMSetProperty(inst, "Reservation",
                              (CMPIValue *)&dev->dev.mem.size, CMPI_uint64);
                CMSetProperty(inst, "Limit",
                              (CMPIValue *)&dev->dev.mem.maxsize, CMPI_uint64);
        } else if (dev->type == VIRT_DEV_VCPU) {
                proc_rasd_from_vdev(broker, dev, host, ref, inst);
        }

        /* FIXME: Put the HostResource in place */

        free(id);

        return inst;
}

CMPIInstance *get_rasd_instance(const CMPIContext *context,
                                const CMPIObjectPath *ref,
                                const CMPIBroker *broker,
                                const char *id,
                                const uint16_t type)
{
        CMPIInstance *inst = NULL;
        CMPIStatus s;
        int ret;
        char *host = NULL;
        char *devid = NULL;
        virConnectPtr conn = NULL;
        struct virt_device *dev;

        ret = parse_fq_devid((char *)id, &host, &devid);
        if (!ret)
                return NULL;

        conn = connect_by_classname(broker, CLASSNAME(ref), &s);
        if (conn == NULL)
                goto out;

        dev = find_dev(conn, type, host, devid);
        if (dev)
                inst = rasd_from_vdev(broker, dev, host, ref);

 out:
        virConnectClose(conn);
        free(host);
        free(devid);

        return inst;
}

CMPIrc rasd_type_from_classname(const char *cn, uint16_t *type)
{
       char *base = NULL;
       CMPIrc rc = CMPI_RC_ERR_FAILED;

       base = class_base_name(cn);
       if (base == NULL)
                goto out;

       if (STREQ(base, "DiskResourceAllocationSettingData"))
               *type = CIM_RASD_TYPE_DISK;
       else if (STREQ(base, "NetResourceAllocationSettingData"))
               *type = CIM_RASD_TYPE_NET;
       else if (STREQ(base, "ProcResourceAllocationSettingData"))
               *type = CIM_RASD_TYPE_PROC;
       else if (STREQ(base, "MemResourceAllocationSettingData"))
               *type = CIM_RASD_TYPE_MEM;
       else
               goto out;

       rc = CMPI_RC_OK;

 out:
       free(base);

       return rc;
}

CMPIrc rasd_classname_from_type(uint16_t type, const char **classname)
{
        CMPIrc rc = CMPI_RC_OK;
        
        switch(type) {
        case CIM_RASD_TYPE_MEM:
                *classname = "MemResourceAllocationSettingData";
                break;
        case CIM_RASD_TYPE_PROC:
                *classname = "ProcResourceAllocationSettingData";
                break;
        case CIM_RASD_TYPE_NET:
                *classname = "NetResourceAllocationSettingData";
                break;
        case CIM_RASD_TYPE_DISK: 
                *classname = "DiskResourceAllocationSettingData";
                break;
        default:
                rc = CMPI_RC_ERR_FAILED;
        }
        
        return rc;
}

static CMPIStatus GetInstance(CMPIInstanceMI *self,
                              const CMPIContext *context,
                              const CMPIResult *results,
                              const CMPIObjectPath *ref,
                              const char **properties)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        CMPIInstance *inst;
        const char *id = NULL;
        uint16_t type;

        if (cu_get_str_path(ref, "InstanceID", &id) != CMPI_RC_OK) {
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_FAILED,
                           "Missing InstanceID");
                goto out;
        }

        if (rasd_type_from_classname(CLASSNAME(ref), &type) != CMPI_RC_OK) {
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_FAILED,
                           "Unable to determine RASD type");
                goto out;
        }

        inst = get_rasd_instance(context, ref, _BROKER, id, type);

        if (inst != NULL)
                CMReturnInstance(results, inst);
        else
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_NOT_FOUND,
                           "No such instance (%s)", id);
 out:
        return s;
}

int rasds_for_domain(const CMPIBroker *broker,
                     const char *name,
                     const uint16_t type,
                     const CMPIObjectPath *ref,
                     struct inst_list *_list)
{
        struct virt_device *list;
        int count;
        int i;
        virConnectPtr conn;
        CMPIStatus s;

        conn = connect_by_classname(broker, CLASSNAME(ref), &s);
        if (conn == NULL)
                return 0;

        count = list_devs(conn, type, name, &list);

        for (i = 0; i < count; i++) {
                CMPIInstance *inst;

                inst = rasd_from_vdev(broker, &list[i], name, ref);
                if (inst != NULL)
                        inst_list_add(_list, inst);
        }

        if (count > 0)
                cleanup_virt_devices(&list, count);

        virConnectClose(conn);

        return count;
}

DEFAULT_CI();
DEFAULT_MI();
DEFAULT_DI();
DEFAULT_EI();
DEFAULT_EIN();
DEFAULT_INST_CLEANUP();
DEFAULT_EQ();

STD_InstanceMIStub(, 
                   Virt_RASD,
                   _BROKER, 
                   libvirt_cim_init());

/*
 * Local Variables:
 * mode: C
 * c-set-style: "K&R"
 * tab-width: 8
 * c-basic-offset: 8
 * indent-tabs-mode: nil
 * End:
 */
