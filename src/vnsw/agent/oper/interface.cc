/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 sact*/

#include "base/os.h"
#include <sys/types.h>
#include <net/ethernet.h>
#include <netinet/ether.h>
#include <boost/uuid/uuid_io.hpp>
#include <tbb/mutex.h>

#include "base/logging.h"
#include "base/address.h"
#include "db/db.h"
#include "db/db_entry.h"
#include "db/db_table.h"
#include "ifmap/ifmap_node.h"

#include <init/agent_param.h>
#include <cfg/cfg_init.h>
#include <oper/operdb_init.h>
#include <oper/route_common.h>
#include <oper/vm.h>
#include <oper/vn.h>
#include <oper/vrf.h>
#include <oper/nexthop.h>
#include <oper/mpls.h>
#include <oper/mirror_table.h>
#include <oper/interface_common.h>
#include <oper/vrf_assign.h>
#include <oper/vxlan.h>
#include <oper/qos_config.h>
#include <oper/ifmap_dependency_manager.h>
#include <oper/tag.h>
#include <oper/config_manager.h>

#include <vnc_cfg_types.h>
#include <oper/agent_sandesh.h>
#include <oper/sg.h>
#include <sandesh/sandesh_trace.h>
#include <sandesh/common/vns_types.h>
#include <sandesh/common/vns_constants.h>
#include <filter/acl.h>
#include <filter/policy_set.h>
#include <resource_manager/resource_manager.h>
#include <resource_manager/resource_table.h>
#include <resource_manager/vm_interface_index.h>
using namespace std;
using namespace boost::uuids;
using boost::assign::map_list_of;
using boost::assign::list_of;

InterfaceTable *InterfaceTable::interface_table_;

/////////////////////////////////////////////////////////////////////////////
// Interface Table routines
/////////////////////////////////////////////////////////////////////////////
InterfaceTable::InterfaceTable(DB *db, const std::string &name) :
        AgentOperDBTable(db, name), operdb_(NULL), agent_(NULL),
        index_table_(), vmi_count_(0), li_count_(0), active_vmi_count_(0),
        vmi_ifnode_to_req_(0), li_ifnode_to_req_(0), pi_ifnode_to_req_(0) {
    global_config_change_walk_ref_ =
        AllocWalker(boost::bind(&InterfaceTable::L2VmInterfaceWalk,
                                this, _1, _2),
                    boost::bind(&InterfaceTable::VmInterfaceWalkDone,
                                  this, _1, _2));
}

void InterfaceTable::Init(OperDB *oper) {
    operdb_ = oper;
    agent_ = oper->agent();
}

void InterfaceTable::RegisterDBClients(IFMapDependencyManager *dep) {
}

bool InterfaceTable::IFNodeToUuid(IFMapNode *node, boost::uuids::uuid &u) {
    if (strcmp(node->table()->Typename(), "virtual-machine-interface") == 0) {
        return VmiIFNodeToUuid(node, u);
    }

    if (strcmp(node->table()->Typename(), "logical-interface") == 0) {
        return LogicalInterfaceIFNodeToUuid(node, u);
    }

    return false;
}

bool InterfaceTable::IFNodeToReq(IFMapNode *node, DBRequest &req,
        const boost::uuids::uuid &u) {
    if (strcmp(node->table()->Typename(), "physical-interface") == 0) {
        return PhysicalInterfaceIFNodeToReq(node, req, u);
    }

    if (strcmp(node->table()->Typename(), "logical-interface") == 0) {
        return LogicalInterfaceIFNodeToReq(node, req, u);
    }

    if (strcmp(node->table()->Typename(), "virtual-machine-interface") == 0) {
        return VmiIFNodeToReq(node, req, u);
    }

    return false;
}

bool InterfaceTable::InterfaceCommonProcessConfig(IFMapNode *node,
                                                  DBRequest &req,
                                                  const boost::uuids::uuid &u) {
    if (req.oper == DBRequest::DB_ENTRY_DELETE || node->IsDeleted()) {
        return true;
    }

    InterfaceData *data = static_cast<InterfaceData *>(req.data.get());
    IFMapNode *adj_node = agent()->config_manager()->
        FindAdjacentIFMapNode(node, "logical-router");
    if (adj_node) {
        autogen::LogicalRouter *lr =
            static_cast<autogen::LogicalRouter *>(adj_node->GetObject());
        autogen::IdPermsType id_perms = lr->id_perms();
        CfgUuidSet(id_perms.uuid.uuid_mslong, id_perms.uuid.uuid_lslong,
                       data->logical_router_uuid_);
    }

    return true;
}

bool InterfaceTable::ProcessConfig(IFMapNode *node, DBRequest &req,
        const boost::uuids::uuid &u) {
    bool config_processed = false;
    if (strcmp(node->table()->Typename(), "physical-interface") == 0) {
        if (PhysicalInterfaceProcessConfig(node, req, u)) {
            config_processed = true;
        }
    }

    if (strcmp(node->table()->Typename(), "logical-interface") == 0) {
        if (LogicalInterfaceProcessConfig(node, req, u)) {
            config_processed = true;
        }
    }

    if (strcmp(node->table()->Typename(), "virtual-machine-interface") == 0) {
        if (VmiProcessConfig(node, req, u)) {
            config_processed = true;
        }
    }

    //Interface type was identified, if not no need to fill common interface
    //data.
    if (config_processed) {
        InterfaceCommonProcessConfig(node, req, u);
    }
    return config_processed;
}

std::auto_ptr<DBEntry> InterfaceTable::AllocEntry(const DBRequestKey *k) const{
    const InterfaceKey *key = static_cast<const InterfaceKey *>(k);

    return std::auto_ptr<DBEntry>(static_cast<DBEntry *>
                                  (key->AllocEntry(this)));
}

void InterfaceTable::FreeInterfaceId(size_t index) {
    agent()->resource_manager()->Release(Resource::INTERFACE_INDEX, index);
    index_table_.Remove(index);
}

DBEntry *InterfaceTable::OperDBAdd(const DBRequest *req) {
    InterfaceKey *key = static_cast<InterfaceKey *>(req->key.get());
    InterfaceData *data = static_cast<InterfaceData *>(req->data.get());

    Interface *intf = key->AllocEntry(this, data);
    if (intf == NULL)
        return NULL;
    if (intf->type_ == Interface::VM_INTERFACE) {
        vmi_count_++;
    } else  if (intf->type_ == Interface::LOGICAL) {
        li_count_++;
    }
    ResourceManager::KeyPtr rkey(new VmInterfaceIndexResourceKey
                                 (agent()->resource_manager(), key->uuid_,
                                  key->name_));
    intf->id_ = static_cast<IndexResourceData *>(agent()->resource_manager()->
                                                 Allocate(rkey).get())->index();
    index_table_.InsertAtIndex(intf->id_, intf);

    intf->transport_ = data->transport_;
    // Get the os-ifindex and mac of interface
    intf->GetOsParams(agent());

    intf->Add();

    intf->SendTrace(this, Interface::ADD);
    return intf;
}

bool InterfaceTable::OperDBOnChange(DBEntry *entry, const DBRequest *req) {
    bool ret = false;
    InterfaceKey *key = static_cast<InterfaceKey *>(req->key.get());

    switch (key->type_) {
    case Interface::VM_INTERFACE: {
        VmInterface *intf = static_cast<VmInterface *>(entry);
        ret = intf->OnChange(static_cast<VmInterfaceData *>(req->data.get()));
        break;
    }
    case Interface::INET: {
        InetInterface *intf = static_cast<InetInterface *>(entry);
        if (intf) {
            // Get the os-ifindex and mac of interface
            intf->GetOsParams(agent());
            intf->OnChange(static_cast<InetInterfaceData *>(req->data.get()));
            ret = true;
        }
        break;
    }

    case Interface::PHYSICAL: {
        PhysicalInterface *intf = static_cast<PhysicalInterface *>(entry);
        ret = intf->OnChange(this, static_cast<PhysicalInterfaceData *>
                             (req->data.get()));
        break;
    }

    case Interface::REMOTE_PHYSICAL: {
        RemotePhysicalInterface *intf =
            static_cast<RemotePhysicalInterface *>(entry);
        ret = intf->OnChange(this, static_cast<RemotePhysicalInterfaceData *>
                             (req->data.get()));
        break;
    }

    case Interface::LOGICAL: {
        LogicalInterface *intf = static_cast<LogicalInterface *>(entry);
        ret = intf->OnChange(this, static_cast<LogicalInterfaceData *>
                             (req->data.get()));
        break;
    }

    case Interface::PACKET: {
         PacketInterface *intf = static_cast<PacketInterface *>(entry);
         PacketInterfaceData *data = static_cast<PacketInterfaceData *>(req->data.get());
         ret = intf->OnChange(data);
         break;
    }

    default:
        break;
    }

    return ret;
}

// RESYNC supported only for VM_INTERFACE
bool InterfaceTable::OperDBResync(DBEntry *entry, const DBRequest *req) {
    InterfaceKey *key = static_cast<InterfaceKey *>(req->key.get());

    //RESYNC for QOS config handling for vhost and fabric interface
    InterfaceQosConfigData *qos_config_data =
        dynamic_cast<InterfaceQosConfigData *>(req->data.get());
    if (qos_config_data) {
        Interface *intf = static_cast<Interface *>(entry);
        AgentQosConfigKey key(qos_config_data->qos_config_uuid_);

        AgentQosConfig *qos_config = static_cast<AgentQosConfig *>
            (agent()->qos_config_table()->FindActiveEntry(&key));

        if (intf->qos_config_ != qos_config) {
            intf->qos_config_ = qos_config;
            return true;
        }
        return false;
    }

    if (key->type_ != Interface::VM_INTERFACE)
        return false;

    VmInterfaceData *vm_data = static_cast<VmInterfaceData *>(req->data.get());
    VmInterface *intf = static_cast<VmInterface *>(entry);
    return intf->Resync(this, vm_data);
}

bool InterfaceTable::OperDBDelete(DBEntry *entry, const DBRequest *req) {
    Interface *intf = static_cast<Interface *>(entry);
    bool ret = false;

    if (intf->Delete(req)) {
        intf->SendTrace(this, Interface::DEL);
        ret = true;
    }
    return ret;
}

VrfEntry *InterfaceTable::FindVrfRef(const string &name) const {
    VrfKey key(name);
    return static_cast<VrfEntry *>
        (agent_->vrf_table()->FindActiveEntry(&key));
}

VmEntry *InterfaceTable::FindVmRef(const uuid &uuid) const {
    VmKey key(uuid);
    return static_cast<VmEntry *>(agent_->vm_table()->FindActiveEntry(&key));
}

VnEntry *InterfaceTable::FindVnRef(const uuid &uuid) const {
    VnKey key(uuid);
    return static_cast<VnEntry *>(agent_->vn_table()->FindActiveEntry(&key));
}

MirrorEntry *InterfaceTable::FindMirrorRef(const string &name) const {
    MirrorEntryKey key(name);
    return static_cast<MirrorEntry *>
        (agent_->mirror_table()->FindActiveEntry(&key));
}

DBTableBase *InterfaceTable::CreateTable(DB *db, const std::string &name) {
    interface_table_ = new InterfaceTable(db, name);
    (static_cast<DBTable *>(interface_table_))->Init();
    return interface_table_;
};

Interface *InterfaceTable::FindInterface(size_t index) {
    Interface *intf = index_table_.At(index);
    if (intf && intf->IsDeleted() != true) {
        return intf;
    }
    return NULL;
}

bool InterfaceTable::FindVmUuidFromMetadataIp(const Ip4Address &ip,
                                              std::string *vm_ip,
                                              std::string *vm_uuid,
                                              std::string *vm_project_uuid) {
    Interface *intf = FindInterfaceFromMetadataIp(ip);
    if (intf && intf->type() == Interface::VM_INTERFACE) {
        const VmInterface *vintf = static_cast<const VmInterface *>(intf);
        *vm_ip = vintf->primary_ip_addr().to_string();
        if (vintf->vm()) {
            *vm_uuid = UuidToString(vintf->vm()->GetUuid());
            *vm_project_uuid = UuidToString(vintf->vm_project_uuid());
            return true;
        }
    }
    return false;
}

Interface *InterfaceTable::FindInterfaceFromMetadataIp(const Ip4Address &ip) {
    uint32_t addr = ip.to_ulong();
    if ((addr & 0xFFFF0000) != (METADATA_IP_ADDR & 0xFFFF0000))
        return NULL;
    return index_table_.At(addr & 0xFFFF);
}

void InterfaceTable::VmPortToMetaDataIp(uint32_t index, uint32_t vrfid,
                                        Ip4Address *addr) {
    uint32_t ip = METADATA_IP_ADDR & 0xFFFF0000;
    ip += (index & 0xFFFF);
    *addr = Ip4Address(ip);
}

bool InterfaceTable::L2VmInterfaceWalk(DBTablePartBase *partition,
                                       DBEntryBase *entry) {
    Interface *intf = static_cast<Interface *>(entry);
    if ((intf->type() != Interface::VM_INTERFACE) || intf->IsDeleted())
        return true;

    VmInterface *vm_intf = static_cast<VmInterface *>(entry);
    const VnEntry *vn = vm_intf->vn();
    if (!vm_intf->IsActive())
        return true;

    if (!vn) {
        return true;
    }

    VmInterfaceGlobalVrouterData data(vn->bridging(),
                                 vn->layer3_forwarding(),
                                 vn->GetVxLanId());
    vm_intf->Resync(this, &data);
    return true;
}

void InterfaceTable::VmInterfaceWalkDone(DBTable::DBTableWalkRef walk_ref,
                                         DBTableBase *partition) {
}

void InterfaceTable::GlobalVrouterConfigChanged() {
    WalkAgain(global_config_change_walk_ref_);
}

void InterfaceTable::Clear() {
    AgentDBTable::Clear();
    ReleaseWalker(global_config_change_walk_ref_);
    global_config_change_walk_ref_ = NULL;
}

InterfaceConstRef InterfaceTable::FindVmi(const boost::uuids::uuid &vmi_uuid) {
    VmInterfaceKey key(AgentKey::ADD_DEL_CHANGE, vmi_uuid, "");
    Interface *intf = static_cast<Interface *>(Find(&key, false));
    InterfaceConstRef ref(intf);
    return ref;
}

void InterfaceTable::CreateVhost() {
    if (agent()->tsn_enabled()) {
        Interface::Transport transport = static_cast<Interface::Transport>
            (agent()->GetInterfaceTransport());
        AgentParam *params = agent()->params();
        InetInterface::Create(this, agent()->vhost_interface_name(),
                              InetInterface::VHOST,
                              agent()->fabric_vrf_name(),
                              params->vhost_addr(),
                              params->vhost_plen(),
                              params->gateway_list()[0],
                              params->eth_port_list()[0],
                              agent()->fabric_vn_name(), transport);
    } else {
        DBRequest req(DBRequest::DB_ENTRY_ADD_CHANGE);
        req.key.reset(new VmInterfaceKey(AgentKey::ADD_DEL_CHANGE,
                                         nil_uuid(),
                                         agent()->vhost_interface_name()));

        VmInterfaceConfigData *data = new VmInterfaceConfigData(agent(), NULL);
        data->CopyVhostData(agent());
        data->disable_policy_ = true;
        req.data.reset(data);
        Process(req);
    }
}

void InterfaceTable::CreateVhostReq() {
    if (agent()->tsn_enabled()) {
        Interface::Transport transport = static_cast<Interface::Transport>
            (agent()->GetInterfaceTransport());
        InetInterface::CreateReq(this, agent()->vhost_interface_name(),
                                 InetInterface::VHOST,
                                 agent()->fabric_vrf_name(),
                                 agent()->router_id(),
                                 agent()->vhost_prefix_len(),
                                 agent()->vhost_default_gateway()[0],
                                 Agent::NullString(), agent_->fabric_vrf_name(),
                                 transport);
    } else {
        DBRequest req(DBRequest::DB_ENTRY_ADD_CHANGE);
        req.key.reset(new VmInterfaceKey(AgentKey::ADD_DEL_CHANGE, nil_uuid(),
                                         agent()->vhost_interface_name()));

        VmInterfaceConfigData *data = new VmInterfaceConfigData(agent(), NULL);
        data->CopyVhostData(agent());

        req.data.reset(data);
        Enqueue(&req);
    }
}
/////////////////////////////////////////////////////////////////////////////
// Interface Base Entry routines
/////////////////////////////////////////////////////////////////////////////
Interface::Interface(Type type, const uuid &uuid, const string &name,
                     VrfEntry *vrf, bool state,
                     const boost::uuids::uuid &logical_router_uuid) :
    type_(type), uuid_(uuid),
    vrf_(vrf, this), label_(MplsTable::kInvalidLabel),
    l2_label_(MplsTable::kInvalidLabel), ipv4_active_(true),
    ipv6_active_(false), is_hc_active_(true),
    metadata_ip_active_(true), metadata_l2_active_(true),
    l2_active_(true), id_(kInvalidIndex), dhcp_enabled_(true), dhcp_enabled_v6_(true),
    dns_enabled_(true),
    admin_state_(true), test_oper_state_(true), transport_(TRANSPORT_INVALID),
    os_params_(name, kInvalidIndex, state),
    logical_router_uuid_(logical_router_uuid) {
}

Interface::~Interface() {
    InterfaceTable *table = static_cast<InterfaceTable *>(get_table());
    if (id_ != kInvalidIndex) {
        table->FreeInterfaceId(id_);
        id_ = kInvalidIndex;
        if (type_ == VM_INTERFACE) {
            table->decr_vmi_count();
        } else if (type_ == LOGICAL) {
            table->decr_li_count();
        }
    }
}

void Interface::SetPciIndex(Agent *agent, size_t index) {
    std::istringstream pci(agent->params()->physical_interface_pci_addr_list()[index]);

    uint32_t  domain, bus, device, function;
    char c;
    if (pci >> std::hex >> domain) {
        pci >> c;
    } else {
        assert(0);
    }

    if (pci >> std::hex >> bus) {
        pci >> c;
    } else {
        assert(0);
    }

    if (pci >> std::hex >> device) {
        pci >> c;
    } else {
        assert(0);
    }

    pci >> std::hex >> function;
    os_params_.os_index_ = domain << 16 | bus << 8 | device << 3 | function;
    os_params_.os_oper_state_ = true;
}

void Interface::GetOsParams(Agent *agent) {
    if (agent->test_mode()) {
        static int dummy_ifindex = 0;
        if (os_params_.os_index_ == kInvalidIndex) {
            os_params_.os_index_ = ++dummy_ifindex;
            os_params_.mac_.Zero();
            os_params_.mac_.last_octet() = os_params_.os_index_;
        }
        os_params_.os_oper_state_ = test_oper_state_;
        return;
    }

    std::string lookup_name = name();
    const PhysicalInterface *phy_intf = dynamic_cast<const PhysicalInterface *>(this);
    if (phy_intf) {
        lookup_name = phy_intf->display_name();
    }

    size_t index = 0;
    if (transport_ == TRANSPORT_PMD && type_ == PHYSICAL) {
        //PCI address is the name of the interface
        // os index from that
       std::vector<std::string>::const_iterator ptr;
       for (ptr = agent->fabric_interface_name_list().begin();
            ptr != agent->fabric_interface_name_list().end(); ++ptr) {
           if (*ptr == lookup_name) {
               break;
           }
           index++;
       }
       SetPciIndex(agent, index);
    }

    //In case of DPDK, set mac-address to the physical
    //mac address set in configuration file, since
    //agent cane query for mac address as physical interface
    //will not be present
    const VmInterface *vm_intf = dynamic_cast<const VmInterface *>(this);
    if (transport_ == TRANSPORT_PMD) {
        struct ether_addr *addr = NULL;
        if (agent->is_l3mh()) {
            if (phy_intf) {
                addr = ether_aton(agent->params()->
                        physical_interface_mac_addr_list()[index].c_str());
            }  else if (vm_intf && vm_intf->vmi_type() == VmInterface::VHOST) {
                addr = ether_aton(agent->vrrp_mac().ToString().c_str());
            }
        } else {
            if (phy_intf || (vm_intf && vm_intf->vmi_type() == VmInterface::VHOST)) {
                addr = ether_aton(agent->params()->
                        physical_interface_mac_addr_list()[0].c_str());
            }
        }
        if (addr) {
            os_params_.mac_ = *addr;
        } else {
            LOG(ERROR,
                    "Physical interface MAC not set in DPDK vrouter agent");
        }
        return;
    }

    if (transport_ != TRANSPORT_ETHERNET) {
        if (!agent->isVmwareMode()) {
            os_params_.os_oper_state_ = true;
        }
        return;
    }

    ObtainOsSpecificParams(lookup_name, agent);
}

void Interface::SetKey(const DBRequestKey *key) {
    const InterfaceKey *k = static_cast<const InterfaceKey *>(key);
    type_ = k->type_;
    uuid_ = k->uuid_;
    os_params_.name_ = k->name_;
}

uint32_t Interface::vrf_id() const {
    VrfEntryRef p = vrf_;
    if (p == NULL) {
        return VrfEntry::kInvalidIndex;
    }

    return p->vrf_id();
}

void InterfaceTable::set_update_floatingip_cb(UpdateFloatingIpFn fn) {
    update_floatingip_cb_ = fn;
}

const InterfaceTable::UpdateFloatingIpFn &InterfaceTable::update_floatingip_cb()
    const {
    return update_floatingip_cb_;
}

/////////////////////////////////////////////////////////////////////////////
// Pkt Interface routines
/////////////////////////////////////////////////////////////////////////////
PacketInterface::PacketInterface(const std::string &name) :
    Interface(Interface::PACKET, nil_uuid(), name, NULL, true, nil_uuid()) {
}

PacketInterface::~PacketInterface() {
}

DBEntryBase::KeyPtr PacketInterface::GetDBRequestKey() const {
    InterfaceKey *key = new PacketInterfaceKey(uuid_, name());
    return DBEntryBase::KeyPtr(key);
}

void PacketInterface::PostAdd() {
    InterfaceTable *table = static_cast<InterfaceTable *>(get_table());
    InterfaceNH::CreatePacketInterfaceNh(table->agent(), name());
}

bool PacketInterface::Delete(const DBRequest *req) {
    flow_key_nh_= NULL;
    return true;
}

// Enqueue DBRequest to create a Pkt Interface
void PacketInterface::CreateReq(InterfaceTable *table,
                                const std::string &ifname,
                                Interface::Transport transport) {
    DBRequest req(DBRequest::DB_ENTRY_ADD_CHANGE);
    req.key.reset(new PacketInterfaceKey(nil_uuid(), ifname));
    req.data.reset(new PacketInterfaceData(transport));
    table->Enqueue(&req);
}

void PacketInterface::Create(InterfaceTable *table, const std::string &ifname,
                             Interface::Transport transport) {
    DBRequest req(DBRequest::DB_ENTRY_ADD_CHANGE);
    req.key.reset(new PacketInterfaceKey(nil_uuid(), ifname));
    req.data.reset(new PacketInterfaceData(transport));
    table->Process(req);
}

// Enqueue DBRequest to delete a Pkt Interface
void PacketInterface::DeleteReq(InterfaceTable *table,
                                const std::string &ifname) {
    DBRequest req(DBRequest::DB_ENTRY_DELETE);
    req.key.reset(new PacketInterfaceKey(nil_uuid(), ifname));
    req.data.reset(NULL);
    table->Enqueue(&req);
}

void PacketInterface::Delete(InterfaceTable *table, const std::string &ifname) {
    DBRequest req(DBRequest::DB_ENTRY_DELETE);
    req.key.reset(new PacketInterfaceKey(nil_uuid(), ifname));
    req.data.reset(NULL);
    table->Process(req);
}

bool PacketInterface::OnChange(PacketInterfaceData *data) {
    return false;
}
/////////////////////////////////////////////////////////////////////////////
// DHCP Snoop routines
// DHCP Snoop entry can be added from 3 different places,
// - Interface added from config
// - Address learnt from DHCP Snoop on fabric interface
// - Address learnt from vrouter when agent restarts
//
// DHCP Snoop entry is deleted from 2 places
// - Interface deleted from config
// - Audit of entries read from vrouter on restart and config table
/////////////////////////////////////////////////////////////////////////////

// Get DHCP IP address. First try to find entry in DHCP Snoop table.
// If no entry in DHCP Snoop table, query the InterfaceKScan table.
//
// InterfaceKScan table is populated on agent restart
const Ip4Address InterfaceTable::GetDhcpSnoopEntry(const std::string &ifname) {
    tbb::mutex::scoped_lock lock(dhcp_snoop_mutex_);
    const DhcpSnoopIterator it = dhcp_snoop_map_.find(ifname);
    if (it != dhcp_snoop_map_.end()) {
        return it->second.addr_;
    }

    return Ip4Address(0);
}

void InterfaceTable::DeleteDhcpSnoopEntry(const std::string &ifname) {
    tbb::mutex::scoped_lock lock(dhcp_snoop_mutex_);
    const DhcpSnoopIterator it = dhcp_snoop_map_.find(ifname);
    if (it == dhcp_snoop_map_.end()) {
        return;
    }

    dhcp_snoop_map_.erase(it);
}

// Set config_seen_ flag in DHCP Snoop entry.
// Create the DHCP Snoop entry, if not already present
void InterfaceTable::DhcpSnoopSetConfigSeen(const std::string &ifname) {
    tbb::mutex::scoped_lock lock(dhcp_snoop_mutex_);
    const DhcpSnoopIterator it = dhcp_snoop_map_.find(ifname);
    Ip4Address addr(0);

    if (it != dhcp_snoop_map_.end()) {
        addr = it->second.addr_;
    }
    dhcp_snoop_map_[ifname] = DhcpSnoopEntry(addr, true);
}

void InterfaceTable::AddDhcpSnoopEntry(const std::string &ifname,
                                       const Ip4Address &addr) {
    tbb::mutex::scoped_lock lock(dhcp_snoop_mutex_);
    DhcpSnoopEntry entry(addr, false);
    const DhcpSnoopIterator it = dhcp_snoop_map_.find(ifname);

    if (it != dhcp_snoop_map_.end()) {
        // Retain config_entry_ flag from old entry
        if (it->second.config_entry_) {
            entry.config_entry_ = true;
        }

        // If IP address is not specified, retain old IP address
        if (addr.to_ulong() == 0) {
            entry.addr_ = it->second.addr_;
        }
    }

    dhcp_snoop_map_[ifname] = entry;
}

// Audit DHCP Snoop table. Delete the entries which are not seen from config
void InterfaceTable::AuditDhcpSnoopTable() {
    tbb::mutex::scoped_lock lock(dhcp_snoop_mutex_);
    DhcpSnoopIterator it = dhcp_snoop_map_.begin();
    while (it != dhcp_snoop_map_.end()){
        DhcpSnoopIterator del_it = it++;
        if (del_it->second.config_entry_ == false) {
            dhcp_snoop_map_.erase(del_it);
        }
    }
}

/////////////////////////////////////////////////////////////////////////////
// Sandesh routines
/////////////////////////////////////////////////////////////////////////////
static string DeviceTypeToString(VmInterface::DeviceType type) {
    if (type == VmInterface::LOCAL_DEVICE) {
        return "Gateway";
    } else if (type == VmInterface::TOR) {
        return "TOR";
    } else if (type == VmInterface::VM_ON_TAP) {
        return "Tap";
    } else if (type == VmInterface::VM_VLAN_ON_VMI) {
        return "VMI vlan-sub-if";
    } else if (type == VmInterface::REMOTE_VM_VLAN_ON_VMI) {
        return "Remote VM";
    }
    return "Invalid";
}

static string VmiTypeToString(VmInterface::VmiType type) {
    if (type == VmInterface::INSTANCE) {
        return "Virtual Machine";
    } else if (type == VmInterface::SERVICE_CHAIN) {
        return "Service Chain";
    } else if (type == VmInterface::SERVICE_INSTANCE) {
        return "Service Instance";
    } else if (type == VmInterface::BAREMETAL) {
        return "Baremetal";
    } else if (type == VmInterface::GATEWAY) {
        return "Gateway";
    } else if (type == VmInterface::REMOTE_VM) {
        return "Remote VM";
    } else if (type == VmInterface::SRIOV) {
        return "Sriov";
    } else if (type == VmInterface::VHOST) {
        return "VHOST";
    }
    return "Invalid";
}

void Interface::SetItfSandeshData(ItfSandeshData &data) const {
    data.set_index(id_);
    data.set_name(name());
    data.set_uuid(UuidToString(uuid_));

    if (vrf_)
        data.set_vrf_name(vrf_->GetName());
    else
        data.set_vrf_name("--ERROR--");

    if (IsUveActive()) {
        data.set_active("Active");
    } else {
        data.set_active("Inactive");
    }

    if (ipv4_active_) {
        data.set_ipv4_active("Active");
    } else {
        data.set_ipv4_active("Inactive");
    }

    if (is_hc_active_) {
        data.set_health_check_active("Active");
    } else {
        data.set_health_check_active("Inactive");
    }

    if (metadata_ip_active_) {
        data.set_metadata_ip_active("Active");
    } else {
        data.set_metadata_ip_active("Inactive");
    }

    if (ipv6_active_) {
        data.set_ip6_active("Active");
    } else {
        data.set_ip6_active("Inactive");
    }

    if (l2_active_) {
        data.set_l2_active("L2 Active");
    } else {
        data.set_l2_active("L2 Inactive");
    }

    if (dhcp_enabled_) {
        data.set_dhcp_service("Enable");
    } else {
        data.set_dhcp_service("Disable");
    }

    if (dhcp_enabled_v6_) {
        data.set_dhcp_service_v6("Enable");
    } else {
        data.set_dhcp_service_v6("Disable");
    }


    if (dns_enabled_) {
        data.set_dns_service("Enable");
    } else {
        data.set_dns_service("Disable");
    }
    data.set_label(label_);
    data.set_l2_label(l2_label_);
    if (flow_key_nh()) {
        data.set_flow_key_idx(flow_key_nh()->id());
    }
    /* For optional fields set the default values here. This will overwritten
     * (if required) based on interface type */
    data.set_ip6_addr("--NA--");
    std::vector<StaticRouteSandesh> aap_list;
    data.set_allowed_address_pair_list(aap_list);
    data.set_subnet("--NA--");
    data.set_sub_type("--NA--");
    data.set_vrf_assign_acl_uuid("--NA--");
    data.set_vmi_type("--NA--");
    data.set_flood_unknown_unicast(false);

    if (qos_config_.get()) {
        data.set_qos_config(UuidToString(qos_config_->uuid()));
    }

    switch (type_) {
    case Interface::PHYSICAL:
        {
            const PhysicalInterface *pintf =
                    static_cast<const PhysicalInterface *>(this);
            PhysicalInterface::BondChildIntfMapIterator it =
                pintf->getBondChildIntfMap().begin();
            vector<BondInterface> bond_interface_list;
            for(; it != pintf->getBondChildIntfMap().end(); it++)
            {
                PhysicalInterface::Bond_ChildIntf bond_intf;
                bond_intf = it->second;
                BondInterface entry;
                entry.set_intf_name(bond_intf.intf_name);
                entry.set_intf_drv_name(bond_intf.intf_drv_name);
                entry.set_intf_status(bond_intf.intf_status ? "UP" : "DOWN");
                bond_interface_list.push_back(entry);
            }
            data.set_bond_interface_list(bond_interface_list);

            if(pintf->os_params_.os_oper_state_)
                data.set_active("Active");
            else
                data.set_active("Inactive <Oper-state-down>");
            data.set_type("eth");

        }
        break;

    case Interface::REMOTE_PHYSICAL:
        data.set_type("remote-physical-port");
        data.set_vrf_name("--NA--");
        break;

    case Interface::LOGICAL:
        {
            const LogicalInterface *lintf =
                static_cast<const LogicalInterface*>(this);
            data.set_type("logical-port");
            data.set_vrf_name("--NA--");
            data.set_physical_device(lintf->phy_dev_display_name());
            data.set_physical_interface(lintf->phy_intf_display_name());
        }
        break;

    case Interface::VM_INTERFACE: {
        data.set_type("vport");
        const VmInterface *vintf = static_cast<const VmInterface *>(this);
        if (vintf->vn())
            data.set_vn_name(vintf->vn()->GetName());
        if (vintf->vm())
            data.set_vm_uuid(UuidToString(vintf->vm()->GetUuid()));
        data.set_ip_addr(vintf->primary_ip_addr().to_string());
        data.set_ip6_addr(vintf->primary_ip6_addr().to_string());
        data.set_mac_addr(vintf->vm_mac().ToString());
        data.set_mdata_ip_addr(vintf->mdata_ip_addr().to_string());
        data.set_vxlan_id(vintf->vxlan_id());
        if (vintf->policy_enabled()) {
            data.set_policy("Enable");
        } else {
            data.set_policy("Disable");
        }
        data.set_flood_unknown_unicast(vintf->flood_unknown_unicast());

        string common_reason = "";
        if (IsUveActive() == false) {

            if (!vintf->admin_state()) {
                common_reason += "admin-down ";
            }

            if (vintf->vn() == NULL) {
                common_reason += "vn-null ";
            } else if (!vintf->vn()->admin_state()) {
                common_reason += "vn-admin-down ";
            }

            if (vintf->vrf() == NULL) {
                common_reason += "vrf-null ";
            }

            if (vintf->NeedDevice()) {
                if (vintf->os_index() == Interface::kInvalidIndex) {
                    common_reason += "no-dev ";
                }

                if (vintf->os_oper_state() == false) {
                    common_reason += "os-state-down ";
                }
            } else if (vintf->NeedOsStateWithoutDevice()) {
                if (vintf->os_oper_state() == false) {
                    common_reason += "os-state-down ";
                }
            }

            if (vintf->parent() && vintf->parent()->IsActive() == false) {
                common_reason += "parent-inactive ";
            }

            string total_reason = common_reason;
            if (!ipv4_active_) {
                total_reason += "ipv4_inactive ";
            }
            if (!ipv6_active_) {
                total_reason += "ipv6_inactive ";
            }
            if (!l2_active_) {
                total_reason += "l2_inactive ";
            }
            string reason = "Inactive < " + total_reason + " >";
            data.set_active(reason);
        }
        if (!ipv4_active_ || !ipv6_active_) {
            string v4_v6_common_reason = common_reason;
            if (vintf->layer3_forwarding() == false) {
                v4_v6_common_reason += "l3-disabled ";
            }

            if (!ipv4_active_) {
                string reason = "Ipv4 Inactive < " + v4_v6_common_reason;
                if (vintf->primary_ip_addr().to_ulong() == 0) {
                    reason += "no-ipv4-addr ";
                }
                reason += " >";
                data.set_ipv4_active(reason);
            }
            if (!ipv6_active_) {
                string reason = "Ipv6 Inactive < " + v4_v6_common_reason;
                if (vintf->primary_ip6_addr().is_unspecified()) {
                    reason += "no-ipv6-addr ";
                }
                reason += " >";
                data.set_ip6_active(reason);
            }
        }

        if (!l2_active_) {
            string l2_reason = common_reason;
            if (vintf->bridging() == false) {
                l2_reason += "l2-disabled ";
            }
            string reason = "L2 Inactive < " + l2_reason;
            reason += " >";
            data.set_l2_active(reason);
        }

        std::vector<FloatingIpSandeshList> fip_list;
        VmInterface::FloatingIpSet::const_iterator it =
            vintf->floating_ip_list().list_.begin();
        while (it != vintf->floating_ip_list().list_.end()) {
            const VmInterface::FloatingIp &ip = *it;
            FloatingIpSandeshList entry;

            entry.set_ip_addr(ip.floating_ip_.to_string());
            if (ip.vrf_.get()) {
                entry.set_vrf_name(ip.vrf_.get()->GetName());
            } else {
                entry.set_vrf_name("--ERROR--");
            }

            if (ip.Installed()) {
                entry.set_installed("Y");
            } else {
                entry.set_installed("N");
            }
            entry.set_fixed_ip(ip.fixed_ip_.to_string());

            string dir = "";
            switch (it->direction()) {
            case VmInterface::FloatingIp::DIRECTION_BOTH:
                dir = "both";
                break;

            case VmInterface::FloatingIp::DIRECTION_INGRESS:
                dir = "ingress";
                break;

            case VmInterface::FloatingIp::DIRECTION_EGRESS:
                dir = "egress";
                break;

            default:
                dir = "INVALID";
                break;
            }
            entry.set_direction(dir);

            entry.set_port_map_enabled(it->port_map_enabled());
            std::vector<SandeshPortMapping> pmap_list;
            VmInterface::FloatingIp::PortMapIterator pmap_it =
                it->src_port_map_.begin();
            while (pmap_it != it->src_port_map_.end()) {
                SandeshPortMapping pmap;
                pmap.set_protocol(pmap_it->first.protocol_);
                pmap.set_port(pmap_it->first.port_);
                pmap.set_nat_port(pmap_it->second);
                pmap_list.push_back(pmap);
                pmap_it++;
            }
            entry.set_port_map(pmap_list);

            fip_list.push_back(entry);
            it++;
        }
        data.set_fip_list(fip_list);

        std::vector<AliasIpSandeshList> aip_list;
        VmInterface::AliasIpSet::const_iterator a_it =
            vintf->alias_ip_list().list_.begin();
        while (a_it != vintf->alias_ip_list().list_.end()) {
            const VmInterface::AliasIp &ip = *a_it;
            AliasIpSandeshList entry;
            entry.set_ip_addr(ip.alias_ip_.to_string());
            if (ip.vrf_.get()) {
                entry.set_vrf_name(ip.vrf_.get()->GetName());
            } else {
                entry.set_vrf_name("--ERROR--");
            }

            if (ip.Installed()) {
                entry.set_installed("Y");
            } else {
                entry.set_installed("N");
            }
            aip_list.push_back(entry);
            a_it++;
        }
        data.set_alias_ip_list(aip_list);

        data.set_logical_interface_uuid(to_string(vintf->logical_interface()));

        // Add Service VLAN list
        std::vector<ServiceVlanSandeshList> vlan_list;
        VmInterface::ServiceVlanSet::const_iterator vlan_it =
            vintf->service_vlan_list().list_.begin();
        while (vlan_it != vintf->service_vlan_list().list_.end()) {
            const VmInterface::ServiceVlan *vlan = vlan_it.operator->();
            ServiceVlanSandeshList entry;

            entry.set_tag(vlan->tag_);
            if (vlan->vrf_.get()) {
                entry.set_vrf_name(vlan->vrf_.get()->GetName());
            } else {
                entry.set_vrf_name("--ERROR--");
            }
            entry.set_ip_addr(vlan->addr_.to_string());
            entry.set_ip6_addr(vlan->addr6_.to_string());
            entry.set_label(vlan->label_);

            if (vlan->v4_rt_installed_ || vlan->v6_rt_installed_) {
                entry.set_installed("Y");
            } else {
                entry.set_installed("N");
            }
            if (vlan->v4_rt_installed_) {
                entry.set_v4_route_installed("Y");
            } else {
                entry.set_v4_route_installed("N");
            }
            if (vlan->v6_rt_installed_) {
                entry.set_v6_route_installed("Y");
            } else {
                entry.set_v6_route_installed("N");
            }
            vlan_list.push_back(entry);
            vlan_it++;
        }

        std::vector<StaticRouteSandesh> static_route_list;
        VmInterface::StaticRouteSet::iterator static_rt_it =
            vintf->static_route_list().list_.begin();
        while (static_rt_it != vintf->static_route_list().list_.end()) {
            const VmInterface::StaticRoute &rt = *static_rt_it;
            StaticRouteSandesh entry;
            entry.set_vrf_name("");
            entry.set_ip_addr(rt.addr_.to_string());
            entry.set_prefix(rt.plen_);
            entry.set_communities(rt.communities_);
            static_rt_it++;
            static_route_list.push_back(entry);
        }
        data.set_static_route_list(static_route_list);

        std::vector<StaticRouteSandesh> aap_list;
        VmInterface::AllowedAddressPairSet::iterator aap_it =
            vintf->allowed_address_pair_list().list_.begin();
        while (aap_it != vintf->allowed_address_pair_list().list_.end()) {
            const VmInterface::AllowedAddressPair &rt = *aap_it;
            StaticRouteSandesh entry;
            entry.set_vrf_name("");
            entry.set_ip_addr(rt.addr_.to_string());
            entry.set_prefix(rt.plen_);
            if (rt.mac_ !=  MacAddress::ZeroMac()) {
                entry.set_mac_addr(rt.mac_.ToString());
                entry.set_label(rt.label_);
            }
            aap_it++;
            aap_list.push_back(entry);
        }
        data.set_allowed_address_pair_list(aap_list);

        std::vector<std::string> fixed_ip4_list;
        vintf->BuildIpStringList(Address::INET, &fixed_ip4_list);
        data.set_fixed_ip4_list(fixed_ip4_list);

        std::vector<std::string> fixed_ip6_list;
        vintf->BuildIpStringList(Address::INET6, &fixed_ip6_list);
        data.set_fixed_ip6_list(fixed_ip6_list);

        std::vector<std::string> fat_flow_list;
        VmInterface::FatFlowEntrySet::iterator fat_flow_it =
            vintf->fat_flow_list().list_.begin();
        while (fat_flow_it != vintf->fat_flow_list().list_.end()) {
            ostringstream str;
            str << (int)fat_flow_it->protocol << ":" << (int)fat_flow_it->port
                << ":" << fat_flow_it->ignore_address;
            fat_flow_list.push_back(str.str());
            fat_flow_it++;
        }
        data.set_fat_flow_list(fat_flow_list);

        if (vintf->fabric_port()) {
            data.set_fabric_port("FabricPort");
        } else {
            data.set_fabric_port("NotFabricPort");
        }
        if (vintf->need_linklocal_ip()) {
            data.set_alloc_linklocal_ip("LL-Enable");
        } else {
            data.set_alloc_linklocal_ip("LL-Disable");
        }
        data.set_service_vlan_list(vlan_list);
        data.set_analyzer_name(vintf->GetAnalyzer());
        data.set_config_name(vintf->cfg_name());

        VmInterface::TagEntrySet::const_iterator tag_it;
        std::vector<VmiTagData> vmi_tag_l;
        const VmInterface::TagEntryList &tag_l = vintf->tag_list();
        for (tag_it = tag_l.list_.begin(); tag_it != tag_l.list_.end();
             ++tag_it) {
            VmiTagData vmi_tag_data;
            if (tag_it->tag_.get()) {
                vmi_tag_data.set_name(tag_it->tag_->name());
                vmi_tag_data.set_id(tag_it->tag_->tag_id());
                TagEntry::PolicySetList::const_iterator aps_it =
                    tag_it->tag_->policy_set_list().begin();
                std::vector<ApplicationPolicySetLink> aps_uuid_list;
                for(; aps_it != tag_it->tag_->policy_set_list().end();
                      aps_it++) {
                    std::string aps_id =
                        UuidToString(aps_it->get()->uuid());
                    ApplicationPolicySetLink apl;
                    apl.set_application_policy_set(aps_id);
                    aps_uuid_list.push_back(apl);
                }

                vmi_tag_data.set_application_policy_set_list(aps_uuid_list);
                vmi_tag_l.push_back(vmi_tag_data);
            }
        }
        data.set_vmi_tag_list(vmi_tag_l);

        VmInterface::SecurityGroupEntrySet::const_iterator sgit;
        std::vector<VmIntfSgUuid> intf_sg_uuid_l;
        const VmInterface::SecurityGroupEntryList &sg_uuid_l = vintf->sg_list();
        for (sgit = sg_uuid_l.list_.begin(); sgit != sg_uuid_l.list_.end();
             ++sgit) {
            VmIntfSgUuid sg_id;
            sg_id.set_sg_uuid(UuidToString(sgit->uuid_));
            intf_sg_uuid_l.push_back(sg_id);
        }
        data.set_sg_uuid_list(intf_sg_uuid_l);

        data.set_vm_name(vintf->vm_name());
        data.set_vm_project_uuid(UuidToString(vintf->vm_project_uuid()));
        data.set_local_preference(vintf->local_preference());

        data.set_tx_vlan_id(vintf->tx_vlan_id());
        data.set_rx_vlan_id(vintf->rx_vlan_id());
        if (vintf->parent()) {
            data.set_parent_interface(vintf->parent()->name());
        }
        if (vintf->subnet().to_ulong() != 0) {
            std::ostringstream str;
            str << vintf->subnet().to_string() << "/"
                << (int)vintf->subnet_plen();
            data.set_subnet(str.str());
        }

        data.set_sub_type(DeviceTypeToString(vintf->device_type()));
        data.set_vmi_type(VmiTypeToString(vintf->vmi_type()));
        data.set_vhostuser_mode(vintf->vhostuser_mode());

        if (vintf->vrf_assign_acl()) {
            std::string vrf_assign_acl;
            vrf_assign_acl.assign(UuidToString(vintf->vrf_assign_acl()->GetUuid()));
            data.set_vrf_assign_acl_uuid(vrf_assign_acl);
        }

        data.set_service_health_check_ip(
                vintf->service_health_check_ip().to_string());
        data.set_drop_new_flows(vintf->drop_new_flows());

        VmInterface::BridgeDomainEntrySet::const_iterator bd_it;
        std::vector<VmIntfBridgeDomainUuid> intf_bd_uuid_l;
        const VmInterface::BridgeDomainList &bd_list =
            vintf->bridge_domain_list();
        for (bd_it = bd_list.list_.begin(); bd_it != bd_list.list_.end(); ++bd_it) {
            VmIntfBridgeDomainUuid bd_id;
            bd_id.set_bridge_domain_uuid(UuidToString(bd_it->uuid_));
            intf_bd_uuid_l.push_back(bd_id);
        }
        data.set_bridge_domain_list(intf_bd_uuid_l);

        std::vector<std::string> policy_set_acl_list;
        FirewallPolicyList::const_iterator fw_it = vintf->fw_policy_list().begin();
        for(; fw_it != vintf->fw_policy_list().end(); fw_it++) {
            policy_set_acl_list.push_back(UuidToString(fw_it->get()->GetUuid()));
        }
        data.set_policy_set_acl_list(policy_set_acl_list);

        std::vector<std::string> policy_set_fwaas_list;
        fw_it = vintf->fwaas_fw_policy_list().begin();
        for(; fw_it != vintf->fwaas_fw_policy_list().end(); fw_it++) {
            policy_set_fwaas_list.push_back(UuidToString(fw_it->get()->GetUuid()));
        }
        data.set_policy_set_fwaas_list(policy_set_fwaas_list);

        std::vector<SecurityLoggingObjectLink> slo_list;
        UuidList::const_iterator sit = vintf->slo_list().begin();
        while (sit != vintf->slo_list().end()) {
            SecurityLoggingObjectLink slo_entry;
            slo_entry.set_slo_uuid(to_string(*sit));
            slo_list.push_back(slo_entry);
            ++sit;
        }
        data.set_slo_list(slo_list);
        data.set_si_other_end_vmi(UuidToString(vintf->si_other_end_vmi()));
        data.set_cfg_igmp_enable(vintf->cfg_igmp_enable());
        data.set_igmp_enabled(vintf->igmp_enabled());
        data.set_max_flows(vintf->max_flows());
        data.set_mac_ip_learning_enable(vintf->mac_ip_learning_enable());
        if (vintf->mac_ip_learning_enable()) {
            std::vector<LearntMacIpSandeshList> mac_ip_list;
            VmInterface::LearntMacIpSet::const_iterator it =
                vintf->learnt_mac_ip_list().list_.begin();
            while (it != vintf->learnt_mac_ip_list().list_.end()) {
                const VmInterface::LearntMacIp &mac_ip = *it;
                LearntMacIpSandeshList entry;
                entry.set_ip_addr(mac_ip.ip_.to_string());
                entry.set_mac_addr(mac_ip.mac_.ToString());
                if (mac_ip.l2_installed_) {
                    entry.set_l2_installed("Y");
                } else {
                    entry.set_l2_installed("N");
                }
                if (mac_ip.l3_installed_) {
                    entry.set_l3_installed("Y");
                } else {
                    entry.set_l3_installed("N");
                }
                mac_ip_list.push_back(entry);
                it++;
            }
            data.set_mac_ip_list(mac_ip_list);
        }

        break;
    }
    case Interface::INET: {
        data.set_type("vhost");
        const InetInterface *intf = static_cast<const InetInterface*>(this);
        if(intf->xconnect()) {
           data.set_physical_interface(intf->xconnect()->name());
        }
        break;
     }
    case Interface::PACKET:
        data.set_type("pkt");
        break;
    default:
        data.set_type("invalid");
        break;
    }
    data.set_os_ifindex(os_index());
    if (admin_state_) {
        data.set_admin_state("Enabled");
    } else {
        data.set_admin_state("Disabled");
    }

    switch (transport_) {
    case TRANSPORT_ETHERNET:{
        data.set_transport("Ethernet");
        break;
    }
    case TRANSPORT_SOCKET: {
        data.set_transport("Socket");
        break;
    }
    case TRANSPORT_PMD: {
        data.set_transport("PMD");
        break;
    }
    default: {
        data.set_transport("Unknown");
        break;
    }
    }
}

bool Interface::DBEntrySandesh(Sandesh *sresp, std::string &name) const {
    ItfResp *resp = static_cast<ItfResp *>(sresp);

    ItfSandeshData data;
    SetItfSandeshData(data);
    std::vector<ItfSandeshData> &list =
            const_cast<std::vector<ItfSandeshData>&>(resp->get_itf_list());
    list.push_back(data);

    return true;
}

void ItfReq::HandleRequest() const {
    AgentSandeshPtr sand(new AgentIntfSandesh(context(), get_type(),
                                              get_name(), get_uuid(),
                                              get_vn(), get_mac(),
                                              get_ipv4_address(),
                                              get_ipv6_address(),
                                              get_parent_uuid(),
                                              get_ip_active(),
                                              get_ip6_active(),
                                              get_l2_active()));
    sand->DoSandesh(sand);
}

AgentSandeshPtr InterfaceTable::GetAgentSandesh(const AgentSandeshArguments *args,
                                                const std::string &context) {
    return AgentSandeshPtr
        (new AgentIntfSandesh(context, args->GetString("type"),
                              args->GetString("name"), args->GetString("uuid"),
                              args->GetString("vn"), args->GetString("mac"),
                              args->GetString("ipv4"), args->GetString("ipv6"),
                              args->GetString("parent_uuid"),
                              args->GetString("ip_active"),
                              args->GetString("ip6_active"),
                              args->GetString("l2_active")));
}

void Interface::SendTrace(const AgentDBTable *table, Trace event) const {
    InterfaceInfo intf_info;
    intf_info.set_name(name());
    intf_info.set_index(id_);

    switch(event) {
    case ADD:
        intf_info.set_op("Add");
        break;
    case DEL:
        intf_info.set_op("Delete");
        break;
    default:
        intf_info.set_op("Unknown");
        break;
    }
    OPER_TRACE_ENTRY(Interface,
                     table,
                     intf_info);
}

bool Interface::ip_active(Address::Family family) const {
    if (family == Address::INET)
        return ipv4_active_;
    else if (family == Address::INET6)
        return ipv6_active_;
    else
        assert(0);
    return false;
}

bool Interface::IsUveActive() const {
    if (ipv4_active() || ipv6_active() || l2_active()) {
        return true;
    }
    return false;
}

void Interface::UpdateOperStateOfSubIntf(const InterfaceTable *table) {
    tbb::mutex::scoped_lock lock(Interface::back_ref_mutex_);
    std::set<IntrusiveReferrer>::const_iterator it = Interface::back_ref_set_.begin();
    for (; it != Interface::back_ref_set_.end(); it++) {
        VmInterface *vm_intf = static_cast<VmInterface *>((*it).first);
        if (vm_intf && vm_intf->parent()) {
           DBRequest req(DBRequest::DB_ENTRY_ADD_CHANGE);
           req.key.reset(new VmInterfaceKey(AgentKey::RESYNC, vm_intf->GetUuid(),
                         vm_intf->name()));
           req.data.reset(new VmInterfaceOsOperStateData(vm_intf->
                                                         os_oper_state()));
           const_cast<InterfaceTable *>(table)->Enqueue(&req);
        }
    }
}

bool Interface::NeedDefaultOsOperStateDisabled(Agent *agent) const {
    if ((transport_ != TRANSPORT_ETHERNET)  && agent->isVmwareMode()) {
        return true;
    }
    return false;
}

/////////////////////////////////////////////////////////////////////////////
// Map of VMI-UUID to VmiType
/////////////////////////////////////////////////////////////////////////////
void InterfaceTable::AddVmiToVmiType(const boost::uuids::uuid &u, int type) {
    vmi_to_vmitype_map_[u] = type;
}

int InterfaceTable::GetVmiToVmiType(const boost::uuids::uuid &u) {
    InterfaceTable::VmiToVmiTypeMap::iterator it = vmi_to_vmitype_map_.find(u);
    if (it == vmi_to_vmitype_map_.end())
        return -1;
    return it->second;
}

void InterfaceTable::DelVmiToVmiType(const boost::uuids::uuid &u) {
    InterfaceTable::VmiToVmiTypeMap::iterator it = vmi_to_vmitype_map_.find(u);
    if (it == vmi_to_vmitype_map_.end())
        return;
    vmi_to_vmitype_map_.erase(it);
}
