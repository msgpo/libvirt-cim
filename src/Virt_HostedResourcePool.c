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
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include<cmpidt.h>
#include<cmpift.h>
#include<cmpimacs.h>

#include <libvirt/libvirt.h>

#include <libcmpiutil/libcmpiutil.h>
#include "misc_util.h"
#include <libcmpiutil/std_association.h>

#include "Virt_HostSystem.h"
#include "Virt_DevicePool.h"

static const CMPIBroker *_BROKER;

static CMPIStatus pool_to_sys(const CMPIObjectPath *ref,
                              struct std_assoc_info *info,
                              struct inst_list *list)
{
        CMPIInstance *host;
        CMPIStatus s = {CMPI_RC_OK, NULL};
        CMPIInstance *inst;

        if (!match_hypervisor_prefix(ref, info))
                return s;

        s = get_pool_inst(_BROKER, ref, &inst);
        if ((s.rc != CMPI_RC_OK) || (inst == NULL))
                return s;

        s = get_host_cs(_BROKER, ref, &host);
        if (s.rc != CMPI_RC_OK)
                return s;

        inst_list_add(list, host);

        return s;
}

static CMPIStatus sys_to_pool(const CMPIObjectPath *ref,
                              struct std_assoc_info *info,
                              struct inst_list *list)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        int i;
        virConnectPtr conn;

        if (!match_hypervisor_prefix(ref, info))
                return s;

        s = validate_host_ref(_BROKER, ref);
        if (s.rc != CMPI_RC_OK)
                return s;

        conn = connect_by_classname(_BROKER, CLASSNAME(ref), &s);
        if (conn == NULL)
                return s;

        for (i = 0; device_pool_names[i]; i++)
                get_pool_by_type(_BROKER,
                                 conn,
                                 device_pool_names[i],
                                 NAMESPACE(ref),
                                 list);

        CMSetStatus(&s, CMPI_RC_OK);

        return s;
}

LIBVIRT_CIM_DEFAULT_MAKEREF()

static char* group_component[] = {
        "Xen_HostSystem",
        "KVM_HostSystem",
        NULL
};

static char* part_component[] = {
        "Xen_ProcessorPool",
        "Xen_MemoryPool",
        "Xen_NetworkPool",
        "Xen_DiskPool",
        "KVM_ProcessorPool",
        "KVM_MemoryPool",
        "KVM_NetworkPool",
        "KVM_DiskPool",
        NULL
};

static char* assoc_classname[] = {
        "Xen_HostedResourcePool",
        "KVM_HostedResourcePool",        
        NULL
};

static struct std_assoc forward = {
        .source_class = (char**)&part_component,
        .source_prop = "PartComponent",

        .target_class = (char**)&group_component,
        .target_prop = "GroupComponent",

        .assoc_class = (char**)&assoc_classname,

        .handler = pool_to_sys,
        .make_ref = make_ref
};

static struct std_assoc backward = {
        .source_class = (char**)&group_component,
        .source_prop = "GroupComponent",

        .target_class = (char**)&part_component,
        .target_prop = "PartComponent",

        .assoc_class = (char**)&assoc_classname,

        .handler = sys_to_pool,
        .make_ref = make_ref
};

static struct std_assoc *assoc_handlers[] = {
        &forward,
        &backward,
        NULL
};

STDA_AssocMIStub(,
                 Virt_HostedResourcePool,
                 _BROKER,
                 libvirt_cim_init(),
                 assoc_handlers);

/*
 * Local Variables:
 * mode: C
 * c-set-style: "K&R"
 * tab-width: 8
 * c-basic-offset: 8
 * indent-tabs-mode: nil
 * End:
 */
