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

#include <cmpidt.h>
#include <cmpift.h>
#include <cmpimacs.h>

#include <libcmpiutil/libcmpiutil.h>
#include <libcmpiutil/std_instance.h>

#include "device_parsing.h"
#include "misc_util.h"
#include "cs_util.h"

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

static CMPIInstance *rasd_from_vdev(const CMPIBroker *broker,
                                    struct virt_device *dev,
                                    const char *host,
                                    const CMPIObjectPath *ref,
                                    const char **properties)
{
        CMPIStatus s;
        CMPIInstance *inst;
        uint16_t type;
        char *base;
        char *id;
        const char *keys[] = {"InstanceID", NULL};

        if (dev->type == CIM_RES_TYPE_DISK) {
                type = CIM_RES_TYPE_DISK;
                base = "DiskResourceAllocationSettingData";
        } else if (dev->type == CIM_RES_TYPE_NET) {
                type = CIM_RES_TYPE_NET;
                base = "NetResourceAllocationSettingData";
        } else if (dev->type == CIM_RES_TYPE_PROC) {
                type = CIM_RES_TYPE_PROC;
                base = "ProcResourceAllocationSettingData";
        } else if (dev->type == CIM_RES_TYPE_MEM) {
                type = CIM_RES_TYPE_MEM;
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

        s = CMSetPropertyFilter(inst, properties, keys);

        if (s.rc != CMPI_RC_OK)
                CU_DEBUG("Unable to set property filter: %d", s.rc);

        id = get_fq_devid((char *)host, dev->id);

        CMSetProperty(inst, "InstanceID",
                      (CMPIValue *)id, CMPI_chars);

        CMSetProperty(inst, "ResourceType",
                      (CMPIValue *)&type, CMPI_uint16);

        if (dev->type == CIM_RES_TYPE_DISK) {
                CMSetProperty(inst,
                              "VirtualDevice",
                              (CMPIValue *)dev->dev.disk.virtual_dev,
                              CMPI_chars);
                CMSetProperty(inst,
                              "Address",
                              (CMPIValue *)dev->dev.disk.source,
                              CMPI_chars);
        } else if (dev->type == CIM_RES_TYPE_NET) {
                CMSetProperty(inst,
                              "NetworkType",
                              (CMPIValue *)dev->dev.net.type,
                              CMPI_chars);
                CMSetProperty(inst,
                              "Address",
                              (CMPIValue *)dev->dev.net.mac,
                              CMPI_chars);
        } else if (dev->type == CIM_RES_TYPE_MEM) {
                const char *units = "KiloBytes";

                CMSetProperty(inst, "AllocationUnits",
                              (CMPIValue *)units, CMPI_chars);
                CMSetProperty(inst, "VirtualQuantity",
                              (CMPIValue *)&dev->dev.mem.size, CMPI_uint64);
                CMSetProperty(inst, "Reservation",
                              (CMPIValue *)&dev->dev.mem.size, CMPI_uint64);
                CMSetProperty(inst, "Limit",
                              (CMPIValue *)&dev->dev.mem.maxsize, CMPI_uint64);
        } else if (dev->type == CIM_RES_TYPE_PROC) {
                /* This would be the place to set the virtualquantity. */
                uint64_t quantity = dev->dev.vcpu.number + 1;
                CMSetProperty(inst, "VirtualQuantity",
                              (CMPIValue *)&quantity, CMPI_uint64);
        }

        /* FIXME: Put the HostResource in place */

        free(id);

        return inst;
}

CMPIStatus get_rasd_by_name(const CMPIBroker *broker,
                            const CMPIObjectPath *reference,
                            const char *name,
                            const uint16_t type,
                            const char **properties,
                            CMPIInstance **_inst)
{
        CMPIInstance *inst = NULL;
        CMPIStatus s = {CMPI_RC_OK, NULL};
        int ret;
        char *host = NULL;
        char *devid = NULL;
        virConnectPtr conn = NULL;
        struct virt_device *dev;

        conn = connect_by_classname(broker, CLASSNAME(reference), &s);
        if (conn == NULL) {
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_NOT_FOUND,
                           "No such instance");
                goto out;
        }
        
        ret = parse_fq_devid((char *)name, &host, &devid);
        if (ret != 1) {
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_NOT_FOUND,
                           "No such instance (%s)", 
                           name);
                goto out;
        }

        dev = find_dev(conn, type, host, devid);
        if (!dev) {
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_NOT_FOUND,
                           "No such instance (%s)", 
                           name);
                goto out;
        }

        inst = rasd_from_vdev(broker, dev, host, reference, properties);
        if (inst == NULL)
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_FAILED,
                           "Failed to set instance properties");
        else
                *_inst = inst;
        
 out:
        virConnectClose(conn);
        free(host);
        free(devid);

        return s;
}

CMPIStatus get_rasd_by_ref(const CMPIBroker *broker,
                           const CMPIObjectPath *reference,
                           const char **properties,
                           CMPIInstance **_inst)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        CMPIInstance *inst = NULL;
        const char *name = NULL;
        uint16_t type;

        if (cu_get_str_path(reference, "InstanceID", &name) != CMPI_RC_OK) {
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_FAILED,
                           "Missing InstanceID");
                goto out;
        }

        if (res_type_from_rasd_classname(CLASSNAME(reference), &type) != CMPI_RC_OK) {
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_FAILED,
                           "Unable to determine RASD type");
                goto out;
        }

        s = get_rasd_by_name(broker, reference, name, type, properties, &inst);
        if (s.rc != CMPI_RC_OK)
                goto out;
        
        s = cu_validate_ref(broker, reference, inst);
        if (s.rc != CMPI_RC_OK)
                goto out;

        *_inst = inst;
        
 out:
        return s;
}

CMPIrc res_type_from_rasd_classname(const char *cn, uint16_t *type)
{
       char *base = NULL;
       CMPIrc rc = CMPI_RC_ERR_FAILED;

       base = class_base_name(cn);
       if (base == NULL)
                goto out;

       if (STREQ(base, "DiskResourceAllocationSettingData"))
               *type = CIM_RES_TYPE_DISK;
       else if (STREQ(base, "NetResourceAllocationSettingData"))
               *type = CIM_RES_TYPE_NET;
       else if (STREQ(base, "ProcResourceAllocationSettingData"))
               *type = CIM_RES_TYPE_PROC;
       else if (STREQ(base, "MemResourceAllocationSettingData"))
               *type = CIM_RES_TYPE_MEM;
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
        case CIM_RES_TYPE_MEM:
                *classname = "MemResourceAllocationSettingData";
                break;
        case CIM_RES_TYPE_PROC:
                *classname = "ProcResourceAllocationSettingData";
                break;
        case CIM_RES_TYPE_NET:
                *classname = "NetResourceAllocationSettingData";
                break;
        case CIM_RES_TYPE_DISK: 
                *classname = "DiskResourceAllocationSettingData";
                break;
        default:
                rc = CMPI_RC_ERR_FAILED;
        }
        
        return rc;
}

static CMPIStatus _get_rasds(const CMPIBroker *broker,
                             const CMPIObjectPath *reference,
                             const virDomainPtr dom,
                             const uint16_t type,
                             const char **properties,
                             struct inst_list *list)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        int count;
        int i;
        struct virt_device *devs = NULL;

        count = get_devices(dom, &devs, type);
        if (count <= 0)
                goto out;

        /* Bit hackish, but for proc we need to cut list down to one. */
        if (type == CIM_RES_TYPE_PROC) {
                struct virt_device *tmp_dev = NULL;
                tmp_dev = calloc(1, sizeof(*tmp_dev));
                tmp_dev = virt_device_dup(&devs[count - 1]);

                tmp_dev->id = strdup("proc");

                for (i = 0; i < count; i++)
                        cleanup_virt_device(&devs[i]);

                free(devs);
                devs = tmp_dev;
                count = 1;
        }

        for (i = 0; i < count; i++) {
                CMPIInstance *dev = NULL;
                const char *host = NULL;

                host = virDomainGetName(dom);
                if (host == NULL) {
                        cleanup_virt_device(&devs[i]);
                        continue;
                }

                dev = rasd_from_vdev(broker,
                                     &devs[i],
                                     host, 
                                     reference,
                                     properties);
                if (dev)
                        inst_list_add(list, dev);

                cleanup_virt_device(&devs[i]);
        }

 out:
        free(devs);
        return s;
}

static CMPIStatus _enum_rasds(const CMPIBroker *broker,
                              const CMPIObjectPath *reference,
                              const virDomainPtr dom,
                              const uint16_t type,
                              const char **properties,
                              struct inst_list *list)
{
        CMPIStatus s;
        int i;

        if (type == CIM_RES_TYPE_ALL) {
                for (i=0; i<CIM_RES_TYPE_COUNT; i++)
                        s = _get_rasds(broker,
                                       reference,
                                       dom, 
                                       cim_res_types[i],
                                       properties,
                                       list);
        }
        else
                s = _get_rasds(broker,
                               reference,
                               dom, 
                               type,
                               properties,
                               list);

        return s;
}

CMPIStatus enum_rasds(const CMPIBroker *broker,
                      const CMPIObjectPath *ref,
                      const char *domain,
                      const uint16_t type,
                      const char **properties,
                      struct inst_list *list)
{
        virConnectPtr conn = NULL;
        virDomainPtr *domains = NULL;
        int count = 1;
        int i;
        CMPIStatus s = {CMPI_RC_OK, NULL};

        conn = connect_by_classname(_BROKER, CLASSNAME(ref), &s);
        if (conn == NULL)
                goto out;

        if (domain) {
                domains = calloc(1, sizeof(virDomainPtr));
                domains[0] = virDomainLookupByName(conn, domain);
        }
        else
                count = get_domain_list(conn, &domains);

        for (i = 0; i < count; i++) {
                _enum_rasds(broker,
                            ref,
                            domains[i], 
                            type,
                            properties,
                            list);
                virDomainFree(domains[i]);
        }

 out:
        virConnectClose(conn);

        return s;
}

static CMPIStatus return_enum_rasds(const CMPIObjectPath *ref,
                                    const CMPIResult *results,
                                    const char **properties,
                                    const bool names_only)
{
        struct inst_list list;
        CMPIStatus s;
        uint16_t type;

        inst_list_init(&list);

        res_type_from_rasd_classname(CLASSNAME(ref), &type);

        s = enum_rasds(_BROKER, ref, NULL, 
                       type, properties, &list);
        if (s.rc != CMPI_RC_OK)
                goto out;

        if (names_only)
                cu_return_instance_names(results, &list);
        else
                cu_return_instances(results, &list);

 out:
        inst_list_free(&list);

        return s;
}

static CMPIStatus EnumInstanceNames(CMPIInstanceMI *self,
                                    const CMPIContext *context,
                                    const CMPIResult *results,
                                    const CMPIObjectPath *reference)
{
        return return_enum_rasds(reference, results, NULL, true);
}

static CMPIStatus EnumInstances(CMPIInstanceMI *self,
                                const CMPIContext *context,
                                const CMPIResult *results,
                                const CMPIObjectPath *reference,
                                const char **properties)
{

        return return_enum_rasds(reference, results, properties, false);
}

static CMPIStatus GetInstance(CMPIInstanceMI *self,
                              const CMPIContext *context,
                              const CMPIResult *results,
                              const CMPIObjectPath *ref,
                              const char **properties)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        CMPIInstance *inst = NULL;

        s = get_rasd_by_ref(_BROKER, ref, properties, &inst);
        if (s.rc != CMPI_RC_OK)
                goto out;

        CMReturnInstance(results, inst);

 out:
        return s;
}

DEFAULT_CI();
DEFAULT_MI();
DEFAULT_DI();
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
