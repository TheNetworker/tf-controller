/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "base/os.h"
#include "testing/gunit.h"

#include <sys/socket.h>
#include <netinet/if_ether.h>
#include <boost/scoped_array.hpp>
#include <base/logging.h>

#include <io/event_manager.h>
#include <cmn/agent_cmn.h>
#include <oper/operdb_init.h>
#include <controller/controller_init.h>
#include <controller/controller_vrf_export.h>
#include <pkt/pkt_init.h>
#include <services/services_init.h>
#include <vrouter/ksync/ksync_init.h>
#include <oper/vrf.h>
#include <pugixml/pugixml.hpp>
#include <services/dhcp_proto.h>
#include <vr_interface.h>
#include <test/test_cmn_util.h>
#include <services/services_sandesh.h>
#include <services/dhcp_lease_db.h>
#include "vr_types.h"

#define MAC_LEN 6
#define CLIENT_REQ_IP "1.2.3.4"
#define CLIENT_REQ_PREFIX "1.2.3.0"
#define CLIENT_REQ_GW "1.2.3.1"
#define MAX_WAIT_COUNT 3000
#define BUF_SIZE 8192
MacAddress src_mac;
MacAddress dest_mac(0x00, 0x11, 0x12, 0x13, 0x14, 0x15);
#define DHCP_RESPONSE_STRING "Server : 1.1.1.200; Subnet mask : 255.255.255.0; Broadcast : 1.1.1.255; Lease time : 4294967295; Gateway : 1.1.1.200; Host Name : vm1; DNS : 1.1.1.200; Domain Name : test.contrail.juniper.net; "
#define HOST_ROUTE_STRING "Host Routes : 10.1.1.0/24 -> 1.1.1.200;10.1.2.0/24 -> 1.1.1.200;150.25.75.0/24 -> 150.25.75.254;192.168.1.128/28 -> 1.1.1.200;"
#define CHANGED_HOST_ROUTE_STRING "Host Routes : 150.2.2.0/24 -> 1.1.1.200;192.1.1.1/28 -> 1.1.1.200;"
#define IPAM_DHCP_OPTIONS_STRING "DNS : 1.2.3.4; Domain Name : test.com; Time Server : 3.2.14.5"
#define SUBNET_DHCP_OPTIONS_STRING "DNS : 11.12.13.14; Domain Name : subnet.com; Time Server : 3.2.14.5; Lease time : 4294967295; Host Routes : 10.1.1.0/24 -> 1.1.1.200;10.1.2.0/24 -> 1.1.1.200;150.25.75.0/24 -> 150.25.75.254;192.168.1.128/28 -> 1.1.1.200;Gateway : 1.2.3.4; Gateway : 5.6.7.8; Gateway : 1.1.1.200;"
#define PORT_DHCP_OPTIONS_STRING "DNS : 21.22.23.24; Time Server : 13.12.14.15; Domain Name : test.com;"
#define PORT_HOST_ROUTE_STRING "Host Routes : 99.2.3.0/24 -> 1.1.1.200;99.5.0.0/16 -> 99.5.0.1;"

#define DHCP_CHECK(condition)                                                  \
                    do {                                                       \
                      usleep(1000);                                            \
                      client->WaitForIdle();                                   \
                      stats = Agent::GetInstance()->GetDhcpProto()->GetStats();\
                      if (++count == MAX_WAIT_COUNT)                           \
                          assert(0);                                           \
                    } while (condition);                                       \

class DhcpTest : public ::testing::Test {
public:
    DhcpTest() : itf_count_(0), gw_itf_id_(-1), lease_db_(NULL) {
        rid_ = Agent::GetInstance()->interface_table()->Register(
                boost::bind(&DhcpTest::ItfUpdate, this, _2));
    }

    ~DhcpTest() {
        Agent::GetInstance()->interface_table()->Unregister(rid_);
    }

    void ItfUpdate(DBEntryBase *entry) {
        Interface *itf = static_cast<Interface *>(entry);
        VmInterface *vmi = NULL;
        if (itf->type() == Interface::VM_INTERFACE)
            vmi = static_cast<VmInterface *>(itf);
        tbb::mutex::scoped_lock lock(mutex_);
        unsigned int i;
        for (i = 0; i < itf_id_.size(); ++i)
            if (itf_id_[i] == itf->id())
                break;
        if (entry->IsDeleted()) {
            if (itf_count_ && i < itf_id_.size()) {
                itf_count_--;
                LOG(DEBUG, "DHCP test : interface deleted " << itf_id_[0]);
                itf_id_.erase(itf_id_.begin()); // we delete in create order
            }
            if (vmi && vmi->vmi_type() == VmInterface::GATEWAY)
                gw_itf_id_ = -1;
        } else {
            if (i == itf_id_.size()) {
                itf_count_++;
                itf_id_.push_back(itf->id());
                LOG(DEBUG, "DHCP test : interface added " << itf->id());
            }
            if (vmi && vmi->vmi_type() == VmInterface::GATEWAY)
                gw_itf_id_ = itf->id();
        }
    }

    uint32_t GetItfCount() {
        tbb::mutex::scoped_lock lock(mutex_);
        return itf_count_;
    }

    void WaitForItfUpdate(unsigned int expect_count) {
        int count = 0;
        while (GetItfCount() != expect_count) {
            if (++count == MAX_WAIT_COUNT)
                assert(0);
            usleep(1000);
        }
    }

    std::size_t GetItfId(int index) {
        tbb::mutex::scoped_lock lock(mutex_);
        return itf_id_[index];
    }

    std::size_t GetGwItfId() {
        tbb::mutex::scoped_lock lock(mutex_);
        return gw_itf_id_;
    }

    std::size_t fabric_interface_id() {
        PhysicalInterfaceKey key(Agent::GetInstance()->params()->eth_port_list()[0].c_str());
        Interface *intf = static_cast<Interface *>
            (Agent::GetInstance()->interface_table()->FindActiveEntry(&key));
        if (intf)
            return intf->id();
        else
            assert(0);
    }

    void CheckSandeshResponse(Sandesh *sandesh, bool check_dhcp_options,
                              const char *host_routes_string,
                              const char *dhcp_option_string,
                              bool check_other_options,
                              const char *other_option_string,
                              bool gateway) {
        CheckSandeshResponse(sandesh, check_dhcp_options, host_routes_string,
                             dhcp_option_string, check_other_options,
                             other_option_string, gateway, "");
    }
    void CheckSandeshResponse(Sandesh *sandesh, bool check_dhcp_options,
                              const char *host_routes_string,
                              const char *dhcp_option_string,
                              bool check_other_options,
                              const char *other_option_string,
                              bool gateway,
                              const char *yiaddr) {
        if (memcmp(sandesh->Name(), "DhcpPktSandesh",
                   strlen("DhcpPktSandesh")) == 0) {
            DhcpPktSandesh *dhcp_pkt = (DhcpPktSandesh *)sandesh;
            DhcpPkt pkt = (dhcp_pkt->get_pkt_list())[3];
            if (check_dhcp_options) {
                if (strlen(host_routes_string) &&
                    pkt.dhcp_hdr.dhcp_options.find(host_routes_string) ==
                    std::string::npos) {
                    assert(0);
                }
                if (strlen(dhcp_option_string) &&
                    pkt.dhcp_hdr.dhcp_options.find(dhcp_option_string) ==
                    std::string::npos) {
                    assert(0);
                }
                // Check that when host routes are specified, GW option is sent
                if (strlen(host_routes_string) &&
                    pkt.dhcp_hdr.dhcp_options.find("Gateway : ") ==
                    std::string::npos) {
                    if (gateway) assert(0);
                }
            }
            if (check_other_options) {
                if (strlen(other_option_string)  &&
                    pkt.dhcp_hdr.other_options.find(other_option_string) ==
                    std::string::npos) {
                    assert(0);
                }
            }
            if (strlen(yiaddr)  &&
                pkt.dhcp_hdr.yiaddr.find(yiaddr) == std::string::npos) {
                assert(0);
            }
        }
    }

    void CheckAllSandeshResponse(Sandesh *sandesh) {
        if (memcmp(sandesh->Name(), "PktStats",
                   strlen("PktStats")) == 0) {
            PktStats *pkt_stats = (PktStats *)sandesh;
            EXPECT_EQ(pkt_stats->get_total_rcvd(), 9);
            EXPECT_EQ(pkt_stats->get_dhcp_rcvd(), 9);
        }
    }

    void ClearPktTrace() {
        // clear existing ptk trace entries
        ClearAllInfo *clear_info = new ClearAllInfo();
        clear_info->HandleRequest();
        client->WaitForIdle();
        clear_info->Release();
    }

    void SendRelayResponse(uint8_t msg_type, uint8_t *options, int num_options,
                           uint32_t yiaddr, uint32_t vmifindex = 0) {
        int len = 512;
        uint8_t *buf = new uint8_t[len];
        memset(buf, 0, len);

        dhcphdr *dhcp = (dhcphdr *) buf;
        dhcp->op = BOOT_REPLY;
        dhcp->htype = HW_TYPE_ETHERNET;
        dhcp->hlen = ETHER_ADDR_LEN;
        dhcp->hops = 0;
        dhcp->xid = 0x01020304;
        dhcp->secs = 0;
        dhcp->flags = 0;
        dhcp->ciaddr = 0;
        dhcp->yiaddr = htonl(yiaddr);
        dhcp->siaddr = 0;
        dhcp->giaddr = 0;
        src_mac.ToArray(dhcp->chaddr, sizeof(dhcp->chaddr));
        memset(dhcp->sname, 0, DHCP_NAME_LEN);
        memset(dhcp->file, 0, DHCP_FILE_LEN);
        len = DHCP_FIXED_LEN;
        len += AddOptions(dhcp->options, msg_type, vmifindex, options, num_options);

        Agent::GetInstance()->GetDhcpProto()->SendDhcpIpc(buf, len);
    }

    void SendDhcp(short ifindex, uint16_t flags, uint8_t msg_type,
                  uint8_t *options, int num_options, char *source_mac,
                  bool error = false, bool response = false,
                  uint32_t yiaddr = 0, uint32_t vmifindex = 0,
                  uint16_t server_port = DHCP_SERVER_PORT) {
        int len = 512;
        uint8_t *buf = new uint8_t[len];
        memset(buf, 0, len);

        struct ether_header *eth = (struct ether_header *)buf;
        eth->ether_dhost[5] = 1;
        eth->ether_shost[5] = 2;
        eth->ether_type = htons(ETHERTYPE_IP);

        agent_hdr *agent = (agent_hdr *)(eth + 1);
        agent->hdr_ifindex = htons(ifindex);
        agent->hdr_vrf = htons(0);
        agent->hdr_cmd = htons(AgentHdr::TRAP_NEXTHOP);

        eth = (struct ether_header *) (agent + 1);
        src_mac = MacAddress(std::string(source_mac));
        dest_mac.ToArray(eth->ether_dhost, sizeof(eth->ether_dhost));
        eth->ether_type = htons(ETHERTYPE_IP);

        struct ip *ip = (struct ip *) (eth + 1);
        ip->ip_hl = 5;
        ip->ip_v = 4;
        ip->ip_tos = 0;
        ip->ip_id = 0;
        ip->ip_off = 0;
        ip->ip_ttl = 16;
        ip->ip_p = IPPROTO_UDP;
        ip->ip_sum = 0;
        if (response) {
            ip->ip_src.s_addr = inet_addr("1.2.3.254");
            ip->ip_dst.s_addr = 0;
        } else {
            ip->ip_src.s_addr = 0;
            ip->ip_dst.s_addr = inet_addr("255.255.255.255");
        }

        udphdr *udp = (udphdr *) (ip + 1);
        if (response) {
            udp->uh_sport = htons(server_port);
            udp->uh_dport = htons(server_port + 1);
        } else {
            udp->uh_sport = htons(server_port + 1);
            udp->uh_dport = htons(server_port);
        }
        udp->uh_sum = 0;

        dhcphdr *dhcp = (dhcphdr *) (udp + 1);
        if (response) {
            dhcp->op = BOOT_REPLY;
        } else {
            dhcp->op = BOOT_REQUEST;
        }
        dhcp->htype = HW_TYPE_ETHERNET;
        dhcp->hlen = ETHER_ADDR_LEN;
        dhcp->hops = 0;
        dhcp->xid = 0x01020304;
        dhcp->secs = 0;
        dhcp->flags = htons(flags);
        dhcp->ciaddr = 0;
        dhcp->yiaddr = htonl(yiaddr);
        dhcp->siaddr = 0;
        dhcp->giaddr = 0;
        src_mac.ToArray(dhcp->chaddr, sizeof(dhcp->chaddr));
        memset(dhcp->sname, 0, DHCP_NAME_LEN);
        memset(dhcp->file, 0, DHCP_FILE_LEN);
        len = sizeof(udphdr) + DHCP_FIXED_LEN;
        len += AddOptions(dhcp->options, msg_type, vmifindex, options, num_options);
        if (error) {
            // send an error message by modifying the DHCP OPTIONS COOKIE
            memcpy(dhcp->options, "1234", 4);
        }

        udp->uh_ulen = htons(len);
        ip->ip_len = htons(len + sizeof(struct ip));
        len += sizeof(struct ip) + sizeof(struct ether_header) +
                Agent::GetInstance()->pkt()->pkt_handler()->EncapHeaderLen();
        TestPkt0Interface *tap = (TestPkt0Interface *)
                (Agent::GetInstance()->pkt()->control_interface());
        tap->TxPacket(buf, len);
    }

    int AddOptions(uint8_t *ptr, uint8_t msg_type, uint32_t ifindex,
                   uint8_t *options, int num_options) {
        memcpy(ptr, DHCP_OPTIONS_COOKIE, 4);
        ptr += 4;
        int len = 4;
        for (int i = 0; i < num_options; i++) {
            *ptr = options[i];
            ptr += 1;
            len += 1;
            switch(options[i]) {
                case DHCP_OPTION_PAD:
                    break;
                case DHCP_OPTION_HOST_NAME:
                    *ptr = 10;
                    memcpy(ptr+1, "host1.test", 10);
                    ptr += 11;
                    len += 11;
                    break;
                case DHCP_OPTION_REQ_IP_ADDRESS:
                    *ptr = 4;
                    *(in_addr_t *)(ptr+1) = inet_addr(CLIENT_REQ_IP);
                    ptr += 5;
                    len += 5;
                    break;
                case DHCP_OPTION_MSG_TYPE:
                    *ptr = 1;
                    *(ptr+1) = msg_type;
                    ptr += 2;
                    len += 2;
                    break;
                case DHCP_OPTION_PARAMETER_REQUEST_LIST:
                    *ptr = 1;
                    *(ptr+1) = DHCP_OPTION_CLASSLESS_ROUTE;
                    ptr += 2;
                    len += 2;
                    break;
                case DHCP_OPTION_DOMAIN_NAME:
                    *ptr = 11;
                    memcpy(ptr+1, "test.domain", 11);
                    ptr += 12;
                    len += 12;
                    break;
                case DHCP_OPTION_82: {
                    *ptr = sizeof(uint32_t) + 2 + sizeof(VmInterface *) + 2;
                    ptr++;
                    *ptr = DHCP_SUBOP_CKTID;
                    *(ptr + 1) = sizeof(uint32_t);
                    *(uint32_t *)(ptr + 2) = htonl(ifindex);
                    ptr += sizeof(uint32_t) + 2;
                    *ptr = DHCP_SUBOP_REMOTEID;
                    *(ptr + 1) = sizeof(VmInterface *);
                    Interface *vm = InterfaceTable::GetInstance()->FindInterface(ifindex);
                    assert(vm != NULL);
                    memcpy(ptr+2, &vm, sizeof(VmInterface *));
                    ptr += sizeof(VmInterface *) + 2;
                    len += sizeof(uint32_t) + 2 + sizeof(VmInterface *) + 2 + 1;
                    break;
                }
                case DHCP_OPTION_END:
                    break;
                default:
                    assert(0);
            }
        }

        return len;
    }

    void DhcpEnableTest(bool order) {
        struct PortInfo input[] = {
            {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
            {"vnet2", 2, "1.1.1.2", "00:00:00:02:02:02", 1, 2},
        };
        uint8_t options[] = {
            DHCP_OPTION_MSG_TYPE,
            DHCP_OPTION_HOST_NAME,
            DHCP_OPTION_DOMAIN_NAME,
            DHCP_OPTION_END
        };
        DhcpProto::DhcpStats stats;

        IpamInfo ipam_info[] = {
            {"1.1.1.0", 24, "1.1.1.200", true},
            {"1.2.3.128", 27, "1.2.3.129", true},
            {"7.8.9.0", 24, "7.8.9.12", false},
        };

        char vdns_attr[] = "<virtual-DNS-data>\n <domain-name>test.contrail.juniper.net</domain-name>\n <dynamic-records-from-client>true</dynamic-records-from-client>\n <record-order>fixed</record-order>\n <default-ttl-seconds>120</default-ttl-seconds>\n </virtual-DNS-data>\n";
        char ipam_attr[] = "<network-ipam-mgmt>\n <ipam-dns-method>virtual-dns-server</ipam-dns-method>\n <ipam-dns-server><virtual-dns-server-name>vdns1</virtual-dns-server-name></ipam-dns-server>\n </network-ipam-mgmt>\n";

        if (order) {
            CreateVmportEnv(input, 2, 0);
            client->WaitForIdle();
            client->Reset();
            AddVDNS("vdns1", vdns_attr);
            client->WaitForIdle();
            AddIPAM("vn1", ipam_info, 3, ipam_attr, "vdns1");
            client->WaitForIdle();
        } else {
            client->Reset();
            AddVDNS("vdns1", vdns_attr);
            client->WaitForIdle();
            AddIPAM("vn1", ipam_info, 3, ipam_attr, "vdns1");
            client->WaitForIdle();
            CreateVmportEnv(input, 2, 0);
            client->WaitForIdle();
        }

        // Check the dhcp_enable flag
        VnEntry *vn = VnGet(1);
        std::vector<VnIpam> vn_ipam = vn->GetVnIpam();
        for (uint32_t i = 0; i < sizeof(ipam_info) / sizeof(IpamInfo); ++i) {
            EXPECT_TRUE(vn_ipam[i].dhcp_enable == ipam_info[i].dhcp_enable);
        }

        MacAddress src_mac(0x00, 0x01, 0x02, 0x03, 0x04, 0x05);
        SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 4, input[0].mac);
        SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 4, input[0].mac);
        int count = 0;
        DHCP_CHECK (stats.acks < 1);
        EXPECT_EQ(1U, stats.discover);
        EXPECT_EQ(1U, stats.request);
        EXPECT_EQ(1U, stats.offers);
        EXPECT_EQ(1U, stats.acks);

        // modify IPAM dhcp_enable
        for (uint32_t i = 0; i < sizeof(ipam_info) / sizeof(IpamInfo); ++i) {
            ipam_info[i].dhcp_enable = !ipam_info[i].dhcp_enable;
        }
        AddIPAM("vn1", ipam_info, 3, ipam_attr, "vdns1");
        client->WaitForIdle();
        vn_ipam = vn->GetVnIpam();
        for (uint32_t i = 0; i < sizeof(ipam_info) / sizeof(IpamInfo); ++i) {
            EXPECT_TRUE(vn_ipam[i].dhcp_enable == ipam_info[i].dhcp_enable);
        }

        // now DHCP should be disabled for 1.1.1.0 subnet
        SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 4, input[0].mac);
        SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 4, input[0].mac);
        client->WaitForIdle();
        count = 0;
        DHCP_CHECK (stats.acks < 1);
        EXPECT_EQ(1U, stats.discover);
        EXPECT_EQ(1U, stats.request);
        EXPECT_EQ(1U, stats.offers);
        EXPECT_EQ(1U, stats.acks);

        client->Reset();
        DelIPAM("vn1", "vdns1");
        client->WaitForIdle();
        DelVDNS("vdns1");
        client->WaitForIdle();

        client->Reset();
        DeleteVmportEnv(input, 2, 1, 0);
        client->WaitForIdle();

        Agent::GetInstance()->GetDhcpProto()->ClearStats();
    }


    void DhcpEnablev6Test(bool order) {
        struct PortInfo input[] = { 
            {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1, "fd15::2"},
            {"vnet2", 2, "7.8.9.2", "00:00:00:02:02:02", 1, 2, "1234::2"},
        };  

        uint8_t options[] = {
            DHCP_OPTION_MSG_TYPE,
            DHCP_OPTION_HOST_NAME,
            DHCP_OPTION_DOMAIN_NAME,
            DHCP_OPTION_END
        };
        DhcpProto::DhcpStats stats;


        IpamInfo ipam_info[] = {
            {"fd15::", 120, "fd15::1", true},
            {"1234::", 64, "1234::1", true},
        };

        char vdns_attr[] = "<virtual-DNS-data>\n <domain-name>test.contrail.juniper.net</domain-name>\n <dynamic-records-from-client>true</dynamic-records-from-client>\n <record-order>fixed</record-order>\n <default-ttl-seconds>120</default-ttl-seconds>\n </virtual-DNS-data>\n";
        char ipam_attr[] = "<network-ipam-mgmt>\n <ipam-dns-method>virtual-dns-server</ipam-dns-method>\n <ipam-dns-server><virtual-dns-server-name>vdns1</virtual-dns-server-name></ipam-dns-server>\n </network-ipam-mgmt>\n";

        if (order) {
            CreateVmportEnv(input, 2, 0);
            client->WaitForIdle();
            client->Reset();
            AddVDNS("vdns1", vdns_attr);
            client->WaitForIdle();
            AddIPAM("vn1", ipam_info, 2, ipam_attr, "vdns1");
            client->WaitForIdle();
        } else {
            client->Reset();
            AddVDNS("vdns1", vdns_attr);
            client->WaitForIdle();
            AddIPAM("vn1", ipam_info, 2, ipam_attr, "vdns1");
            client->WaitForIdle();
            CreateVmportEnv(input, 2, 0);
            client->WaitForIdle();
        }

        // Check the dhcp_enable_v6 flag
        VnEntry *vn = VnGet(1);
        std::vector<VnIpam> vn_ipam = vn->GetVnIpam();
        for (uint32_t i = 0; i < sizeof(ipam_info) / sizeof(IpamInfo); ++i) {
            EXPECT_TRUE(vn_ipam[i].dhcp_enable_v6 == ipam_info[i].dhcp_enable);
        }


        MacAddress src_mac(0x00, 0x01, 0x02, 0x03, 0x04, 0x05);
        SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 4, input[0].mac);
        SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 4, input[0].mac);
        int count = 0;
        DHCP_CHECK (stats.acks < 1);
        EXPECT_EQ(1U, stats.discover);
        EXPECT_EQ(1U, stats.request);
        EXPECT_EQ(1U, stats.offers);
        EXPECT_EQ(1U, stats.acks);

        VmInterface *vmi_0 = VmInterfaceGet(1);
        vmi_0->set_dhcp_enable_v6_config(ipam_info[0].dhcp_enable);
        bool ret = vmi_0->dhcp_enable_v6_config();
        EXPECT_EQ(ret, true);

        // modify IPAM dhcp_enable_v6
        for (uint32_t i = 0; i < sizeof(ipam_info) / sizeof(IpamInfo); ++i) {
            ipam_info[i].dhcp_enable = !ipam_info[i].dhcp_enable;
        }
        AddIPAM("vn1", ipam_info, 2, ipam_attr, "vdns1");
        client->WaitForIdle();
        vn_ipam = vn->GetVnIpam();
        for (uint32_t i = 0; i < sizeof(ipam_info) / sizeof(IpamInfo); ++i) {
            EXPECT_TRUE(vn_ipam[i].dhcp_enable_v6 == ipam_info[i].dhcp_enable);
        }

        VmInterface *vmi_1 = VmInterfaceGet(1);
        vmi_1->set_dhcp_enable_v6_config(ipam_info[0].dhcp_enable);
        ret = vmi_1->dhcp_enable_v6_config();
        EXPECT_EQ(ret, false);

        client->Reset();
        DelIPAM("vn1", "vdns1");
        client->WaitForIdle();
        DelVDNS("vdns1");
        client->WaitForIdle();

        client->Reset();
        DeleteVmportEnv(input, 2, 1, 0);
        client->WaitForIdle();

        Agent::GetInstance()->GetDhcpProto()->ClearStats(); 

    }

    void DhcpOptionCategoryTest(char *vm_interface_attr,
                                bool dhcp_string, const char *dhcp_option_string,
                                bool other_string, const char *other_option_string) {
        struct PortInfo input[] = {
            {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
        };
        uint8_t options[] = {
            DHCP_OPTION_MSG_TYPE,
            DHCP_OPTION_HOST_NAME,
            DHCP_OPTION_DOMAIN_NAME,
            DHCP_OPTION_END
        };
        DhcpProto::DhcpStats stats;

        IpamInfo ipam_info[] = {
            {"1.2.3.128", 27, "1.2.3.129", true},
            {"7.8.9.0", 24, "7.8.9.12", true},
            {"1.1.1.0", 24, "1.1.1.200", true},
        };

        CreateVmportEnv(input, 1, 0, NULL, NULL, vm_interface_attr);
        client->WaitForIdle();
        client->Reset();
        AddIPAM("vn1", ipam_info, 3);
        client->WaitForIdle();

        ClearPktTrace();
        SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 4, input[0].mac);
        SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 4, input[0].mac);
        int count = 0;
        DHCP_CHECK (stats.acks < 1);
        EXPECT_EQ(1U, stats.discover);
        EXPECT_EQ(1U, stats.request);
        EXPECT_EQ(1U, stats.offers);
        EXPECT_EQ(1U, stats.acks);

        DhcpInfo *sand = new DhcpInfo();
        Sandesh::set_response_callback(boost::bind(
            &DhcpTest::CheckSandeshResponse, this, _1,
            dhcp_string, "", dhcp_option_string,
            other_string, other_option_string, true));
        sand->HandleRequest();
        client->WaitForIdle();
        sand->Release();

        client->Reset();
        DelIPAM("vn1", "vdns1");
        client->WaitForIdle();
        DelVDNS("vdns1");
        client->WaitForIdle();

        client->Reset();
        DeleteVmportEnv(input, 1, 1, 0);
        client->WaitForIdle();

        ClearPktTrace();
        Agent::GetInstance()->GetDhcpProto()->ClearStats();
    }

    void LoadDhcpLeaseFile(const Ip4Address &subnet, uint8_t plen,
                           const std::string &name) {
        if (!lease_db_) {
            const std::vector<Ip4Address> reserve_addresses;
            boost::asio::io_service *io =
                Agent::GetInstance()->event_manager()->io_service();
            lease_db_ = new DhcpLeaseDb(subnet, plen, reserve_addresses,
                                        name, *io);
        } else {
            lease_db_->ClearLeases();
            lease_db_->LoadLeaseFile();
        }
    }

    bool CheckDhcpLease(const MacAddress &mac, const Ip4Address &ip,
                        bool released) {
        const std::set<DhcpLeaseDb::DhcpLease> &leases = lease_db_->leases();
        std::set<DhcpLeaseDb::DhcpLease>::const_iterator it =
            leases.find(DhcpLeaseDb::DhcpLease(mac, Ip4Address(), 0, false));
        if (it != leases.end()) {
            if (it->mac_ == mac && it->ip_ == ip && it->released_ == released)
                return true;
        }

        return false;
    }

    void SetDhcpLeaseTimeout(uint32_t timeout) {
        Interface *itf =
            Agent::GetInstance()->interface_table()->FindInterface(gw_itf_id_);
        if (!itf) return;
        DhcpLeaseDb *lease_db =
            Agent::GetInstance()->GetDhcpProto()->GetLeaseDb(itf);
        lease_db->set_lease_timeout(timeout);
    }

    void CloseDhcpLeaseFile() {
        delete lease_db_;
        lease_db_ = NULL;
    }

private:
    DBTableBase::ListenerId rid_;
    uint32_t itf_count_;
    std::vector<std::size_t> itf_id_;
    std::size_t gw_itf_id_;
    tbb::mutex mutex_;
    DhcpLeaseDb *lease_db_;
};

class AsioRunEvent : public Task {
public:
    AsioRunEvent() : Task(75) { };
    virtual  ~AsioRunEvent() { };
    bool Run() {
        Agent::GetInstance()->event_manager()->Run();
        return true;
    }
    std::string Description() const { return "AsioRunEvent"; }
};

TEST_F(DhcpTest, DhcpReqTest) {
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
        {"vnet2", 2, "1.1.1.2", "00:00:00:02:02:02", 1, 2},
    };
    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    ClearPktTrace();
    IpamInfo ipam_info[] = {
        {"1.2.3.128", 27, "1.2.3.129", true},
        {"7.8.9.0", 24, "7.8.9.12", true},
        {"1.1.1.0", 24, "1.1.1.200", true},
    };
    char vdns_attr[] = "<virtual-DNS-data>\n <domain-name>test.contrail.juniper.net</domain-name>\n <dynamic-records-from-client>true</dynamic-records-from-client>\n <record-order>fixed</record-order>\n <default-ttl-seconds>120</default-ttl-seconds>\n </virtual-DNS-data>\n";
    char ipam_attr[] = "<network-ipam-mgmt>\n <ipam-dns-method>virtual-dns-server</ipam-dns-method>\n <ipam-dns-server><virtual-dns-server-name>vdns1</virtual-dns-server-name></ipam-dns-server>\n </network-ipam-mgmt>\n";

    CreateVmportEnv(input, 2, 0);
    client->WaitForIdle();
    client->Reset();
    AddVDNS("vdns1", vdns_attr);
    client->WaitForIdle();
    AddIPAM("vn1", ipam_info, 3, ipam_attr, "vdns1");
    client->WaitForIdle();

    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 4, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 4, input[0].mac);
    int count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);
    SendDhcp(GetItfId(1), 0x8000, DHCP_DISCOVER, options, 4, input[1].mac);
    SendDhcp(GetItfId(1), 0x8000, DHCP_REQUEST, options, 4, input[1].mac);
    SendDhcp(GetItfId(1), 0, DHCP_DISCOVER, options, 4, input[1].mac);
    SendDhcp(GetItfId(1), 0, DHCP_REQUEST, options, 4, input[1].mac);
    count = 0;
    DHCP_CHECK (stats.acks < 3);
    EXPECT_EQ(3U, stats.discover);
    EXPECT_EQ(3U, stats.request);
    EXPECT_EQ(3U, stats.offers);
    EXPECT_EQ(3U, stats.acks);
    SendDhcp(GetItfId(1), 0x8000, DHCP_INFORM, options, 4, input[1].mac);
    SendDhcp(GetItfId(1), 0x8000, DHCP_DECLINE, options, 4, input[1].mac);
    count = 0;
    DHCP_CHECK (stats.decline < 1);
    EXPECT_EQ(3U, stats.discover);
    EXPECT_EQ(3U, stats.request);
    EXPECT_EQ(1U, stats.inform);
    EXPECT_EQ(1U, stats.decline);
    EXPECT_EQ(3U, stats.offers);
    EXPECT_EQ(4U, stats.acks);
    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 4, input[0].mac, true);
    count = 0;
    DHCP_CHECK (stats.errors < 1);
    EXPECT_EQ(3U, stats.discover);
    EXPECT_EQ(3U, stats.request);
    EXPECT_EQ(1U, stats.inform);
    EXPECT_EQ(1U, stats.decline);
    EXPECT_EQ(3U, stats.offers);
    EXPECT_EQ(4U, stats.acks);
    EXPECT_EQ(1U, stats.errors);
    client->WaitForIdle();

    DhcpInfo *sand = new DhcpInfo();
    Sandesh::set_response_callback(
        boost::bind(&DhcpTest::CheckSandeshResponse, this, _1,
                    true, "", DHCP_RESPONSE_STRING, false, "", true));
    sand->HandleRequest();
    client->WaitForIdle();
    sand->Release();

    ShowAllInfo *all_sandesh = new ShowAllInfo();
    Sandesh::set_response_callback(
        boost::bind(&DhcpTest::CheckAllSandeshResponse, this, _1));
    all_sandesh->HandleRequest();
    client->WaitForIdle();
    all_sandesh->Release();

    client->Reset();
    DelIPAM("vn1", "vdns1");
    client->WaitForIdle();
    DelVDNS("vdns1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 2, 1, 0);
    client->WaitForIdle();

    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}

TEST_F(DhcpTest, DhcpOtherReqTest) {
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
    };
    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    CreateVmportEnv(input, 1, 0);
    client->WaitForIdle();
    client->Reset();

    SendDhcp(GetItfId(0), 0x8000, DHCP_RELEASE, options, 2, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_LEASE_QUERY, options, 2, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_ACK, options, 2, input[0].mac);
    int count = 0;
    DHCP_CHECK (stats.other < 2);
    EXPECT_EQ(2U, stats.other);
    EXPECT_EQ(1U, stats.release);

    client->Reset();
    DeleteVmportEnv(input, 1, 1, 0);
    client->WaitForIdle();

    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}

TEST_F(DhcpTest, DhcpOptionTest) {
    struct PortInfo input[] = {
        {"vnet3", 3, CLIENT_REQ_IP, "00:00:00:03:03:03", 1, 3},
    };
    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_REQ_IP_ADDRESS,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_PAD,
        DHCP_OPTION_PAD,
        DHCP_OPTION_PAD,
        DHCP_OPTION_PAD,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    IpamInfo ipam_info[] = {
        {CLIENT_REQ_PREFIX, 24, CLIENT_REQ_GW, true},
    };
    char vdns_attr[] = "<virtual-DNS-data>\n <domain-name>test.domain</domain-name>\n <dynamic-records-from-client>true</dynamic-records-from-client>\n <record-order>fixed</record-order>\n <default-ttl-seconds>120</default-ttl-seconds>\n </virtual-DNS-data>\n";
    char ipam_attr[] =
    "<network-ipam-mgmt>\
        <ipam-dns-method>virtual-dns-server</ipam-dns-method>\
        <ipam-dns-server>\
            <virtual-dns-server-name>vdns1</virtual-dns-server-name>\
        </ipam-dns-server>\
        <dhcp-option-list>\
            <dhcp-option>\
                <dhcp-option-name>6</dhcp-option-name>\
                <dhcp-option-value>1.2.3.4</dhcp-option-value>\
                <dhcp-option-value-bytes>2 2 2 2</dhcp-option-value-bytes>\
            </dhcp-option>\
            <dhcp-option>\
                <dhcp-option-name>15</dhcp-option-name>\
                <dhcp-option-value>test.com</dhcp-option-value>\
            </dhcp-option>\
            <dhcp-option>\
                <dhcp-option-name>4</dhcp-option-name>\
                <dhcp-option-value>3.2.14.5</dhcp-option-value>\
            </dhcp-option>\
            <dhcp-option>\
                <dhcp-option-name>4</dhcp-option-name>\
                <dhcp-option-value>junk</dhcp-option-value>\
            </dhcp-option>\
        </dhcp-option-list>\
    </network-ipam-mgmt>";

    CreateVmportEnv(input, 1, 0);
    client->WaitForIdle();
    client->Reset();
    AddVDNS("vdns1", vdns_attr);
    client->WaitForIdle();
    AddIPAM("vn1", ipam_info, 1, ipam_attr, "vdns1");
    client->WaitForIdle();

    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 9, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 9, input[0].mac);
    int count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

    DhcpInfo *sand = new DhcpInfo();
    Sandesh::set_response_callback(boost::bind(&DhcpTest::CheckSandeshResponse,
                                               this, _1, false, "", "",
                                               false, "", true));
    sand->HandleRequest();
    client->WaitForIdle();
    sand->Release();

    client->Reset();
    DelIPAM("vn1", "vdns1");
    client->WaitForIdle();
    DelVDNS("vdns1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 1, 1, 0);
    client->WaitForIdle();

    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}

TEST_F(DhcpTest, DhcpNakTest) {
    struct PortInfo input[] = {
        {"vnet4", 4, "5.6.7.8", "00:00:00:04:04:04", 1, 4},
    };
    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_REQ_IP_ADDRESS,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    IpamInfo ipam_info[] = {
        {"5.6.7.0", 24, "5.6.7.1", true},
    };
    CreateVmportEnv(input, 1, 0);
    client->WaitForIdle();
    client->Reset();
    AddIPAM("vn1", ipam_info, 1);
    client->WaitForIdle();

    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 4, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 4, input[0].mac);
    int count = 0;
    DHCP_CHECK (stats.nacks < 1);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.nacks);

    DhcpInfo *sand = new DhcpInfo();
    Sandesh::set_response_callback(boost::bind(&DhcpTest::CheckSandeshResponse,
                                               this, _1, false, "", "",
                                               false, "", true));
    sand->HandleRequest();
    client->WaitForIdle();
    sand->Release();

    client->Reset();
    DelIPAM("vn1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 1, 1, 0);
    client->WaitForIdle();

    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}

TEST_F(DhcpTest, DhcpShortLeaseTest) {
    struct PortInfo input[] = {
        {"vnet5", 5, "9.6.7.8", "00:00:00:05:05:05", 1, 5},
    };
    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    IntfCfgAdd(input, 0);
    WaitForItfUpdate(1);

    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 3, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 3, input[0].mac);
    int count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 3, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 3, input[0].mac);
    count = 0;
    DHCP_CHECK (stats.acks < 2);
    EXPECT_EQ(2U, stats.discover);
    EXPECT_EQ(2U, stats.request);
    EXPECT_EQ(2U, stats.offers);
    EXPECT_EQ(2U, stats.acks);

    IpamInfo ipam_info[] = {
        {"9.6.7.0", 24, "9.6.7.254", true},
    };
    CreateVmportEnv(input, 1, 0);
    client->WaitForIdle();
    client->Reset();
    AddIPAM("vn1", ipam_info, 1);
    client->WaitForIdle();

    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 3, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 3, input[0].mac);
    count = 0;
    DHCP_CHECK (stats.acks < 3);
    EXPECT_EQ(3U, stats.discover);
    EXPECT_EQ(3U, stats.request);
    EXPECT_EQ(3U, stats.offers);
    EXPECT_EQ(3U, stats.acks);

    IntfCfgDel(input, 0);
    WaitForItfUpdate(0);

    client->Reset();
    DelIPAM("vn1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 1, 1, 0);
    client->WaitForIdle();

    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}

TEST_F(DhcpTest, DhcpTenantDnsTest) {
    struct PortInfo input[] = {
        {"vnet6", 6, "3.2.5.7", "00:00:00:06:06:06", 1, 6},
    };
    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    IpamInfo ipam_info[] = {
        {"3.2.5.0", 24, "3.2.5.254", true},
    };
    char ipam_attr[] =
    "<network-ipam-mgmt>\
        <ipam-dns-method>tenant-dns-server</ipam-dns-method>\
        <ipam-dns-server>\
            <tenant-dns-server-address>\
                <ip-address>3.2.4.5</ip-address>\
                <ip-address>5.5.4.5</ip-address>\
                <ip-address>junk</ip-address>\
            </tenant-dns-server-address>\
        </ipam-dns-server>\
        <dhcp-option-list>\
            <dhcp-option>\
                <dhcp-option-name>6</dhcp-option-name>\
                <dhcp-option-value>1.2.3.4</dhcp-option-value>\
            </dhcp-option>\
            <dhcp-option>\
                <dhcp-option-name>15</dhcp-option-name>\
                <dhcp-option-value>test.com</dhcp-option-value>\
            </dhcp-option>\
            <dhcp-option>\
                <dhcp-option-name>4</dhcp-option-name>\
                <dhcp-option-value>3.2.14.5</dhcp-option-value>\
            </dhcp-option>\
        </dhcp-option-list>\
    </network-ipam-mgmt>";

    CreateVmportEnv(input, 1, 0);
    client->WaitForIdle();
    client->Reset();
    AddIPAM("vn1", ipam_info, 1, ipam_attr);
    client->WaitForIdle();

    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 4, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 4, input[0].mac);
    int count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

    client->Reset();
    DelIPAM("vn1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 1, 1, 0);
    client->WaitForIdle();

    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}

TEST_F(DhcpTest, DhcpFabricPortTest) {
    struct PortInfo input[] = {
        {"vnet7", 7, "1.1.1.1", "00:00:00:07:07:07", 1, 7},
    };
    Ip4Address vmaddr(Agent::GetInstance()->router_id().to_ulong() + 1);
    strncpy(input[0].addr, vmaddr.to_string().c_str(), 32);
    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    CreateVmportEnv(input, 1, 0, NULL,
                    Agent::GetInstance()->fabric_vrf_name().c_str());
    client->WaitForIdle();

    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 3, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 3, input[0].mac);
    int count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

    client->Reset();
    DeleteVmportEnv(input, 1, 1, 0, NULL,
                    Agent::GetInstance()->fabric_vrf_name().c_str());
    client->WaitForIdle();

    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}

TEST_F(DhcpTest, DhcpZeroIpTest) {
    struct PortInfo input[] = {
        {"vnet8", 8, "0.0.0.0", "00:00:00:08:08:08", 1, 8},
    };
    uint8_t req_options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_END
    };
    uint8_t resp_options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_82,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    Agent::GetInstance()->GetDhcpProto()->set_dhcp_relay_mode(true);
    CreateVmportEnv(input, 1, 0, NULL,
                    Agent::GetInstance()->fabric_vrf_name().c_str());
    client->WaitForIdle();

    Ip4Address vmaddr(Agent::GetInstance()->router_id().to_ulong() + 1);
    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, req_options, 3, input[0].mac);
    // SendDhcp(fabric_interface_id(), 0x8000, DHCP_OFFER, resp_options, 4, false, true, vmaddr.to_ulong(), GetItfId(0));
    SendRelayResponse(DHCP_OFFER, resp_options, 4, vmaddr.to_ulong(), GetItfId(0));
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, req_options, 3, input[0].mac);
    // SendDhcp(fabric_interface_id(), 0x8000, DHCP_ACK, resp_options, 4, false, true, vmaddr.to_ulong(), GetItfId(0));
    SendRelayResponse(DHCP_ACK, resp_options, 4, vmaddr.to_ulong(), GetItfId(0));
    client->WaitForIdle();
    int count = 0;
    DHCP_CHECK (stats.relay_resp < 2);
    EXPECT_EQ(2U, stats.relay_req);
    EXPECT_EQ(2U, stats.relay_resp);
    EXPECT_TRUE(RouteFind(Agent::GetInstance()->fabric_vrf_name(), vmaddr, 32));

    client->Reset();
    DeleteVmportEnv(input, 1, 1, 0, NULL,
                    Agent::GetInstance()->fabric_vrf_name().c_str());
    client->WaitForIdle();

    Agent::GetInstance()->GetDhcpProto()->ClearStats();
    Agent::GetInstance()->GetDhcpProto()->set_dhcp_relay_mode(false);
}

TEST_F(DhcpTest, IpamSpecificDhcpOptions) {
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
        {"vnet2", 2, "1.1.1.2", "00:00:00:02:02:02", 1, 2},
    };
    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    IpamInfo ipam_info[] = {
        {"1.2.3.128", 27, "1.2.3.129", true},
        {"7.8.9.0", 24, "7.8.9.12", true},
        {"1.1.1.0", 24, "1.1.1.200", true},
    };
    char vdns_attr[] = "<virtual-DNS-data>\n <domain-name>test.contrail.juniper.net</domain-name>\n <dynamic-records-from-client>true</dynamic-records-from-client>\n <record-order>fixed</record-order>\n <default-ttl-seconds>120</default-ttl-seconds>\n </virtual-DNS-data>\n";
    char ipam_attr[] =
    "<network-ipam-mgmt>\
        <ipam-dns-method>virtual-dns-server</ipam-dns-method>\
        <ipam-dns-server><virtual-dns-server-name>vdns1</virtual-dns-server-name></ipam-dns-server>\
        <dhcp-option-list>\
            <dhcp-option>\
                <dhcp-option-name>6</dhcp-option-name>\
                <dhcp-option-value>1.2.3.4</dhcp-option-value>\
            </dhcp-option>\
            <dhcp-option>\
                <dhcp-option-name>15</dhcp-option-name>\
                <dhcp-option-value>test.com</dhcp-option-value>\
            </dhcp-option>\
            <dhcp-option>\
                <dhcp-option-name>4</dhcp-option-name>\
                <dhcp-option-value>3.2.14.5</dhcp-option-value>\
            </dhcp-option>\
        </dhcp-option-list>\
        <host-routes>\
            <route><prefix>10.1.1.0/24</prefix> <next-hop /> <next-hop-type /></route>\
            <route><prefix>10.1.2.0/24</prefix> <next-hop>junk</next-hop> <next-hop-type /></route>\
            <route><prefix>150.25.75.0/24</prefix> <next-hop>150.25.75.254</next-hop> <next-hop-type /></route>\
            <route><prefix>192.168.1.128/28</prefix> <next-hop>0.0.0.0</next-hop> <next-hop-type /></route>\
        </host-routes>\
    </network-ipam-mgmt>";

    CreateVmportEnv(input, 2, 0);
    client->WaitForIdle();
    client->Reset();
    AddVDNS("vdns1", vdns_attr);
    client->WaitForIdle();
    AddIPAM("vn1", ipam_info, 3, ipam_attr, "vdns1");
    client->WaitForIdle();

    ClearPktTrace();
    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 4, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 4, input[0].mac);
    int count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

    DhcpInfo *sand = new DhcpInfo();
    Sandesh::set_response_callback(boost::bind(&DhcpTest::CheckSandeshResponse,
                                               this, _1, true,
                                               HOST_ROUTE_STRING,
                                               IPAM_DHCP_OPTIONS_STRING,
                                               false, "", true));
    sand->HandleRequest();
    client->WaitForIdle();
    sand->Release();

    // change host routes
    ClearPktTrace();
    std::vector<std::string> vm_host_routes;
    vm_host_routes.push_back("150.2.2.0/24");
    vm_host_routes.push_back("192.1.1.1/28");
    AddIPAM("vn1", ipam_info, 3, ipam_attr, "vdns1", &vm_host_routes);
    client->WaitForIdle();

    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 4, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 4, input[0].mac);
    count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

    DhcpInfo *new_sand = new DhcpInfo();
    Sandesh::set_response_callback(boost::bind(&DhcpTest::CheckSandeshResponse,
                                               this, _1, true,
                                               CHANGED_HOST_ROUTE_STRING,
                                               IPAM_DHCP_OPTIONS_STRING,
                                               false, "", true));
    new_sand->HandleRequest();
    client->WaitForIdle();
    new_sand->Release();

    client->Reset();
    DelIPAM("vn1", "vdns1");
    client->WaitForIdle();
    DelVDNS("vdns1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 2, 1, 0);
    client->WaitForIdle();

    ClearPktTrace();
    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}

// Check that options at subnet override options at ipam level
TEST_F(DhcpTest, SubnetSpecificDhcpOptions) {
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
        {"vnet2", 2, "1.1.1.2", "00:00:00:02:02:02", 1, 2},
    };
    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    IpamInfo ipam_info[] = {
        {"1.2.3.128", 27, "1.2.3.129", true},
        {"7.8.9.0", 24, "7.8.9.12", true},
        {"1.1.1.0", 24, "1.1.1.200", true},
    };
    char vdns_attr[] = "<virtual-DNS-data>\n <domain-name>test.contrail.juniper.net</domain-name>\n <dynamic-records-from-client>true</dynamic-records-from-client>\n <record-order>fixed</record-order>\n <default-ttl-seconds>120</default-ttl-seconds>\n </virtual-DNS-data>\n";
    char ipam_attr[] =
    "<network-ipam-mgmt>\
        <ipam-dns-method>default-dns-server</ipam-dns-method>\
        <dhcp-option-list>\
            <dhcp-option>\
                <dhcp-option-name>6</dhcp-option-name>\
                <dhcp-option-value>1.2.3.4</dhcp-option-value>\
            </dhcp-option>\
            <dhcp-option>\
                <dhcp-option-name>15</dhcp-option-name>\
                <dhcp-option-value>test.com</dhcp-option-value>\
            </dhcp-option>\
            <dhcp-option>\
                <dhcp-option-name>4</dhcp-option-name>\
                <dhcp-option-value>3.2.14.5</dhcp-option-value>\
            </dhcp-option>\
            <dhcp-option>\
                <dhcp-option-name>3</dhcp-option-name>\
                <dhcp-option-value>12.13.14.15 23.24.25.26</dhcp-option-value>\
            </dhcp-option>\
        </dhcp-option-list>\
        <host-routes>\
            <route><prefix>1.2.3.0/24</prefix> <next-hop /> <next-hop-type /></route>\
            <route><prefix>4.5.0.0/16</prefix> <next-hop>4.5.0.254</next-hop> <next-hop-type /></route>\
        </host-routes>\
    </network-ipam-mgmt>";
    char add_subnet_tags[] =
    "<dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>6</dhcp-option-name>\
            <dhcp-option-value>11.12.13.14</dhcp-option-value>\
        </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>15</dhcp-option-name>\
            <dhcp-option-value>subnet.com</dhcp-option-value>\
        </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>3</dhcp-option-name>\
            <dhcp-option-value>1.2.3.4 5.6.7.8</dhcp-option-value>\
        </dhcp-option>\
     </dhcp-option-list>";
    // option 4 from ipam and other options from subnet should be used

    std::vector<std::string> vm_host_routes;
    vm_host_routes.push_back("10.1.1.0/24");
    vm_host_routes.push_back("10.1.2.0/24 junk");
    vm_host_routes.push_back("150.25.75.0/24 150.25.75.254");
    vm_host_routes.push_back("192.168.1.128/28 0.0.0.0");

    CreateVmportEnv(input, 2, 0);
    client->WaitForIdle();
    client->Reset();
    AddVDNS("vdns1", vdns_attr);
    client->WaitForIdle();
    AddIPAM("vn1", ipam_info, 3, ipam_attr, "vdns1", &vm_host_routes, add_subnet_tags);
    client->WaitForIdle();

    ClearPktTrace();
    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 4, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 4, input[0].mac);
    int count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

    DhcpInfo *sand = new DhcpInfo();
    Sandesh::set_response_callback(boost::bind(&DhcpTest::CheckSandeshResponse,
                                               this, _1, true,
                                               HOST_ROUTE_STRING,
                                               SUBNET_DHCP_OPTIONS_STRING,
                                               false, "", true));
    sand->HandleRequest();
    client->WaitForIdle();
    sand->Release();

    client->Reset();
    DelIPAM("vn1", "vdns1");
    client->WaitForIdle();
    DelVDNS("vdns1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 2, 1, 0);
    client->WaitForIdle();

    ClearPktTrace();
    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}

// Check that options at vm interface override options at subnet & ipam levels
TEST_F(DhcpTest, PortSpecificDhcpOptions) {
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
        {"vnet2", 2, "1.1.1.2", "00:00:00:02:02:02", 1, 2},
    };
    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_PARAMETER_REQUEST_LIST,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    IpamInfo ipam_info[] = {
        {"1.2.3.128", 27, "1.2.3.129", true},
        {"7.8.9.0", 24, "7.8.9.12", true},
        {"1.1.1.0", 24, "1.1.1.200", true},
    };
    char ipam_attr[] =
    "<network-ipam-mgmt>\
        <ipam-dns-method>default-dns-server</ipam-dns-method>\
        <dhcp-option-list>\
            <dhcp-option>\
                <dhcp-option-name>6</dhcp-option-name>\
                <dhcp-option-value>1.2.3.4</dhcp-option-value>\
            </dhcp-option>\
            <dhcp-option>\
                <dhcp-option-name>15</dhcp-option-name>\
                <dhcp-option-value>test.com</dhcp-option-value>\
            </dhcp-option>\
            <dhcp-option>\
                <dhcp-option-name>4</dhcp-option-name>\
                <dhcp-option-value>3.2.14.5</dhcp-option-value>\
            </dhcp-option>\
        </dhcp-option-list>\
        <host-routes>\
            <route><prefix>1.2.3.0/24</prefix> <next-hop>0.0.0.0</next-hop> <next-hop-type /></route>\
            <route><prefix>4.5.0.0/16</prefix> <next-hop>4.5.0.254</next-hop> <next-hop-type /></route>\
        </host-routes>\
    </network-ipam-mgmt>";

    char add_subnet_tags[] =
    "<dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>6</dhcp-option-name>\
            <dhcp-option-value>11.12.13.14</dhcp-option-value>\
        </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>4</dhcp-option-name>\
            <dhcp-option-value>13.12.14.15</dhcp-option-value>\
        </dhcp-option>\
     </dhcp-option-list>";

    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>6</dhcp-option-name>\
            <dhcp-option-value>21.22.23.24</dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>\
     <virtual-machine-interface-host-routes>\
         <route><prefix>99.2.3.0/24</prefix> <next-hop>0.0.0.0</next-hop> <next-hop-type /> </route>\
         <route><prefix>99.5.0.0/16</prefix> <next-hop>99.5.0.1</next-hop> <next-hop-type /> </route>\
    </virtual-machine-interface-host-routes>";
    // option 6 from vm interface, option 4 from subnet and option 15
    // from ipam should be used

    std::vector<std::string> vm_host_routes;
    vm_host_routes.push_back("10.1.1.0/24");
    vm_host_routes.push_back("10.1.2.0/24");
    vm_host_routes.push_back("150.25.75.0/24");
    vm_host_routes.push_back("192.168.1.128/28");

    CreateVmportEnv(input, 2, 0, NULL, NULL, vm_interface_attr);
    client->WaitForIdle();
    client->Reset();
    AddIPAM("vn1", ipam_info, 3, ipam_attr, "vdns1", &vm_host_routes, add_subnet_tags);
    client->WaitForIdle();

    ClearPktTrace();
    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 5, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 5, input[0].mac);
    int count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

    DhcpInfo *sand = new DhcpInfo();
    Sandesh::set_response_callback(boost::bind(&DhcpTest::CheckSandeshResponse,
                                               this, _1, true,
                                               PORT_HOST_ROUTE_STRING,
                                               PORT_DHCP_OPTIONS_STRING,
                                               false, "", false));
    sand->HandleRequest();
    client->WaitForIdle();
    sand->Release();

    client->Reset();
    DelIPAM("vn1", "vdns1");
    client->WaitForIdle();
    DelVDNS("vdns1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 2, 1, 0);
    client->WaitForIdle();

    ClearPktTrace();
    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}

// Check that DHCP requests from TOR are served
#ifdef DHCP_FLAKY
TEST_F(DhcpTest, DhcpTorRequestTest) {
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
        {"vnet2", 2, "1.1.1.2", "00:00:00:02:02:02", 1, 2},
    };
    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    ClearPktTrace();
    IpamInfo ipam_info[] = {
        {"1.2.3.128", 27, "1.2.3.129", true},
        {"7.8.9.0", 24, "7.8.9.12", true},
        {"1.1.1.0", 24, "1.1.1.200", true},
    };
    char vdns_attr[] = "<virtual-DNS-data>\n <domain-name>test.contrail.juniper.net</domain-name>\n <dynamic-records-from-client>true</dynamic-records-from-client>\n <record-order>fixed</record-order>\n <default-ttl-seconds>120</default-ttl-seconds>\n </virtual-DNS-data>\n";
    char ipam_attr[] = "<network-ipam-mgmt>\n <ipam-dns-method>virtual-dns-server</ipam-dns-method>\n <ipam-dns-server><virtual-dns-server-name>vdns1</virtual-dns-server-name></ipam-dns-server>\n </network-ipam-mgmt>\n";

    CreateVmportEnv(input, 2, 0);
    client->WaitForIdle();
    client->Reset();
    AddVDNS("vdns1", vdns_attr);
    client->WaitForIdle();
    AddIPAM("vn1", ipam_info, 3, ipam_attr, "vdns1");
    client->WaitForIdle();

    // use the mac address of the VM as the source MAC
    char mac1[MAC_LEN] = { 0x00, 0x00, 0x00, 0x01, 0x01, 0x01 };
    src_mac = MacAddress(mac1);

    SendDhcp(fabric_interface_id(), 0x8000, DHCP_DISCOVER, options, 4);
    SendDhcp(fabric_interface_id(), 0x8000, DHCP_REQUEST, options, 4);
    int count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

    char mac2[MAC_LEN] = { 0x00, 0x00, 0x00, 0x02, 0x02, 0x02 };
    src_mac = MacAddress(mac2);

    SendDhcp(fabric_interface_id(), 0x8000, DHCP_DISCOVER, options, 4);
    SendDhcp(fabric_interface_id(), 0x8000, DHCP_REQUEST, options, 4);
    count = 0;
    DHCP_CHECK (stats.acks < 2);
    EXPECT_EQ(2U, stats.discover);
    EXPECT_EQ(2U, stats.request);
    EXPECT_EQ(2U, stats.offers);
    EXPECT_EQ(2U, stats.acks);

    SendDhcp(fabric_interface_id(), 0x8000, DHCP_INFORM, options, 4);
    SendDhcp(fabric_interface_id(), 0x8000, DHCP_DECLINE, options, 4);
    count = 0;
    DHCP_CHECK (stats.decline < 1);
    EXPECT_EQ(2U, stats.discover);
    EXPECT_EQ(2U, stats.request);
    EXPECT_EQ(1U, stats.inform);
    EXPECT_EQ(1U, stats.decline);
    EXPECT_EQ(2U, stats.offers);
    EXPECT_EQ(3U, stats.acks);

    char mac3[MAC_LEN] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05 };
    src_mac = MacAddress(mac3);

    SendDhcp(fabric_interface_id(), 0x8000, DHCP_DISCOVER, options, 4, true);
    count = 0;
    DHCP_CHECK (stats.errors < 1);
    EXPECT_EQ(2U, stats.discover);
    EXPECT_EQ(2U, stats.request);
    EXPECT_EQ(1U, stats.inform);
    EXPECT_EQ(1U, stats.decline);
    EXPECT_EQ(2U, stats.offers);
    EXPECT_EQ(3U, stats.acks);
    EXPECT_EQ(1U, stats.errors);
    client->WaitForIdle();

    DhcpInfo *sand = new DhcpInfo();
    Sandesh::set_response_callback(
        boost::bind(&DhcpTest::CheckSandeshResponse, this, _1,
                    true, "", DHCP_RESPONSE_STRING, false, "", true));
    sand->HandleRequest();
    client->WaitForIdle();
    sand->Release();

    client->Reset();
    DelIPAM("vn1", "vdns1");
    client->WaitForIdle();
    DelVDNS("vdns1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 2, 1, 0);
    client->WaitForIdle();

    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}
#endif

TEST_F(DhcpTest, DhcpEnableTestForward) {
    DhcpEnableTest(true);
    DhcpEnablev6Test(true);
}

TEST_F(DhcpTest, DhcpEnableTestReverse) {
    DhcpEnableTest(false);
    DhcpEnablev6Test(false);
}

// Check dhcp options - different categories
TEST_F(DhcpTest, NoDataOption) {
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>rapid-commit</dhcp-option-name>\
            <dhcp-option-value></dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    #define OPTION_CATEGORY_NO_DATA "50 00"
    DhcpOptionCategoryTest(vm_interface_attr, false, "", true,
                           OPTION_CATEGORY_NO_DATA);
}

// Check dhcp options - different categories
TEST_F(DhcpTest, BoolByteOption) {
    // options that take bool value, byte value and byte array
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>ip-forwarding</dhcp-option-name>\
            <dhcp-option-value>1</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>default-ip-ttl</dhcp-option-name>\
            <dhcp-option-value>125</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>vendor-encapsulated-options</dhcp-option-name>\
            <dhcp-option-value-bytes>97 98 99 300</dhcp-option-value-bytes>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>interface-id</dhcp-option-name>\
            <dhcp-option-value>97 98 99 100</dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    // no vendor encapsulated options as value 300 is invalid
    #define OPTION_CATEGORY_BOOL_BYTE "13 01 01 17 01 7d 5e 04 61 62 63 64"
    DhcpOptionCategoryTest(vm_interface_attr, false, "", true,
                           OPTION_CATEGORY_BOOL_BYTE);
}

// Check dhcp options - error input
TEST_F(DhcpTest, BoolByteOptionError) {
    // options that take bool value, byte value and byte array
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>ip-forwarding</dhcp-option-name>\
            <dhcp-option-value>0</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>all-subnets-local</dhcp-option-name>\
            <dhcp-option-value>test</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>all-subnets-local</dhcp-option-name>\
            <dhcp-option-value>300</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>interface-id</dhcp-option-name>\
            <dhcp-option-value></dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>default-ip-ttl</dhcp-option-name>\
            <dhcp-option-value>32</dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    // all-subnets-local is not added as input is bad
    // interface-id option is not added as there is no data
    #define OPTION_CATEGORY_BOOL_BYTE_ERROR "13 01 00 17 01 20 "
    DhcpOptionCategoryTest(vm_interface_attr, false, "", true,
                           OPTION_CATEGORY_BOOL_BYTE_ERROR);
}

// Check that option value in bytes overrides value
TEST_F(DhcpTest, BytesValueOverrideTest) {
    // options that take byte array, Byte String,
    // String, NameCompression as value - override with value in bytes
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>tftp-server-name</dhcp-option-name>\
            <dhcp-option-value>host.juniper.net</dhcp-option-value>\
            <dhcp-option-value-bytes>01 20 100 200</dhcp-option-value-bytes>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>user-class</dhcp-option-name>\
            <dhcp-option-value>10 11 12</dhcp-option-value>\
            <dhcp-option-value-bytes>5 6 7 8</dhcp-option-value-bytes>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>slp-service-scope</dhcp-option-name>\
            <dhcp-option-value>10 abcd</dhcp-option-value>\
            <dhcp-option-value-bytes>12 121 01</dhcp-option-value-bytes>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>domain-search</dhcp-option-name>\
            <dhcp-option-value>test.com juniper.net</dhcp-option-value>\
            <dhcp-option-value-bytes>50 20 68 02</dhcp-option-value-bytes>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    #define OPTION_CATEGORY_BYTES_OVERRIDE "42 04 01 14 64 c8 4d 04 05 06 07 08 4f 03 0c 79 01 77 04 32 14 44 02"
    DhcpOptionCategoryTest(vm_interface_attr, false, "", true,
                           OPTION_CATEGORY_BYTES_OVERRIDE);
}

// Check dhcp options - use option code as dhcp-option-name
TEST_F(DhcpTest, OptionCodeTest) {
    // options that take bool value, byte value and byte array
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>19</dhcp-option-name>\
            <dhcp-option-value>1</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>23</dhcp-option-name>\
            <dhcp-option-value>125</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>94</dhcp-option-name>\
            <dhcp-option-value>97 98 99 100</dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    DhcpOptionCategoryTest(vm_interface_attr, false, "", true,
                           OPTION_CATEGORY_BOOL_BYTE);
}

// Check dhcp options - different categories
TEST_F(DhcpTest, ByteStringOption) {
    // options that take byte value followed by string
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>status-code</dhcp-option-name>\
            <dhcp-option-value>10 value</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>dhcp-vss</dhcp-option-name>\
            <dhcp-option-value>test value</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>dhcp-vss</dhcp-option-name>\
            <dhcp-option-value></dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>dhcp-vss</dhcp-option-name>\
            <dhcp-option-value>3000 wrongvalue</dhcp-option-value>\
        </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>dhcp-client-identifier</dhcp-option-name>\
            <dhcp-option-value>20 abcd</dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    // dhcp-vss shouldnt be present as value was wrong
    #define OPTION_CATEGORY_BYTE_STRING "97 06 0a 76 61 6c 75 65 3d 05 14 61 62 63 64"
    DhcpOptionCategoryTest(vm_interface_attr, false, "", true,
                           OPTION_CATEGORY_BYTE_STRING);
}

// Check dhcp options - different categories
TEST_F(DhcpTest, ByteIpOption) {
    // options that take byte value followed by one or more IP addresses
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>slp-directory-agent</dhcp-option-name>\
            <dhcp-option-value>test 1.2.3.4 5.6.7.8</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>slp-directory-agent</dhcp-option-name>\
            <dhcp-option-value>300 1.2.3.4</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>slp-directory-agent</dhcp-option-name>\
            <dhcp-option-value>1.2.3.4</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>slp-directory-agent</dhcp-option-name>\
            <dhcp-option-value>20 1.2.3.4 5.6.7.8</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>slp-directory-agent</dhcp-option-name>\
            <dhcp-option-value>30 9.8.6.5</dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    #define OPTION_CATEGORY_BYTE_IP "4e 09 14 01 02 03 04 05 06 07 08"
    DhcpOptionCategoryTest(vm_interface_attr, false, "", true,
                           OPTION_CATEGORY_BYTE_IP);
}

// Check dhcp options - different categories
TEST_F(DhcpTest, StringOption) {
    // options that take byte value followed by one or more IP addresses
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>host-name</dhcp-option-name>\
            <dhcp-option-value>test-host</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>domain-name</dhcp-option-name>\
            <dhcp-option-value>test.com</dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    #define OPTION_CATEGORY_STRING "Host Name : test-host; Domain Name : test.com; "
    DhcpOptionCategoryTest(vm_interface_attr, true, OPTION_CATEGORY_STRING,
                           false, "");
}

// Check dhcp options - different categories
TEST_F(DhcpTest, IntOption) {
    // options that take integer values
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>arp-cache-timeout</dhcp-option-name>\
            <dhcp-option-value>100000</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>boot-size</dhcp-option-name>\
            <dhcp-option-value>-1</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>boot-size</dhcp-option-name>\
            <dhcp-option-value>20 30</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>max-dgram-reassembly</dhcp-option-name>\
            <dhcp-option-value></dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>path-mtu-plateau-table</dhcp-option-name>\
            <dhcp-option-value>error value 30 40</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>path-mtu-plateau-table</dhcp-option-name>\
            <dhcp-option-value>30 40 error value</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>path-mtu-plateau-table</dhcp-option-name>\
            <dhcp-option-value>20 30 40</dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    // boot-size, max-dgram-reassembly and initial two path-mtu-plateau-table are ignored (error)
    #define OPTION_CATEGORY_INT "23 04 00 01 86 a0 0d 02 ff ff 19 06 00 14 00 1e 00 28 "
    DhcpOptionCategoryTest(vm_interface_attr, false, "",
                           true, OPTION_CATEGORY_INT);
}

// Check dhcp options - different categories
TEST_F(DhcpTest, IpOption) {
    // options that take IP addresses
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>swap-server</dhcp-option-name>\
            <dhcp-option-value>2.3.4.5</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>router-solicitation-address</dhcp-option-name>\
            <dhcp-option-value>2.3.4.5 4.5.6.7 junk</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>mobile-ip-home-agent</dhcp-option-name>\
            <dhcp-option-value>10.0.1.2 10.1.2.3</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>log-servers</dhcp-option-name>\
            <dhcp-option-value>255.3.3.3</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>slp-directory-agent</dhcp-option-name>\
            <dhcp-option-value></dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>policy-filter</dhcp-option-name>\
            <dhcp-option-value>8.3.2.0 8.3.2.1 3.4.5.6</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>policy-filter</dhcp-option-name>\
            <dhcp-option-value>8.3.2.0 8.3.2.1</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>mobile-ip-home-agent</dhcp-option-name>\
            <dhcp-option-value></dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    // router-solicitation-address is not added as it has to be only one IP
    // slp-directory-agent is not added as it has to be atleast one IP
    // first policy-filter is not added as it has to be multiples of two IPs
    // second mobile-ip-home-agent is not added as it is repeated
    #define OPTION_CATEGORY_IP "10 04 02 03 04 05 44 08 0a 00 01 02 0a 01 02 03 07 04 ff 03 03 03 15 08 08 03 02 00 08 03 02 01"
    DhcpOptionCategoryTest(vm_interface_attr, false, "",
                           true, OPTION_CATEGORY_IP);
}

// Check dhcp options - when DNS server is zero, name servers option shouldnt be sent
TEST_F(DhcpTest, DnsZeroPortOption) {
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>domain-name-servers</dhcp-option-name>\
            <dhcp-option-value>0.0.0.0</dhcp-option-value>\
         </dhcp-option>\
         <dhcp-option>\
             <dhcp-option-name>4</dhcp-option-name>\
             <dhcp-option-value>3.2.14.5</dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    // no DNS in the output
    #define OPTION_DNS_ZERO "Server : 1.1.1.200; Subnet mask : 255.255.255.0; Broadcast : 1.1.1.255; Time Server : 3.2.14.5; Lease time : 4294967295; Gateway : 1.1.1.200; Host Name : vm1; "
    DhcpOptionCategoryTest(vm_interface_attr, true, OPTION_DNS_ZERO,
                           false, "");
}

// Check dhcp options - when DNS server is zero at subnet level, name servers
// option shouldnt be sent even if specified at IPAM level
TEST_F(DhcpTest, DnsZeroSubnetOption) {
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
        {"vnet2", 2, "1.1.1.2", "00:00:00:02:02:02", 1, 2},
    };
    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_PARAMETER_REQUEST_LIST,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    IpamInfo ipam_info[] = {
        {"1.2.3.128", 27, "1.2.3.129", true},
        {"7.8.9.0", 24, "7.8.9.12", true},
        {"1.1.1.0", 24, "1.1.1.200", true},
    };
    char ipam_attr[] =
    "<network-ipam-mgmt>\
        <ipam-dns-method>default-dns-server</ipam-dns-method>\
        <dhcp-option-list>\
            <dhcp-option>\
                <dhcp-option-name>6</dhcp-option-name>\
                <dhcp-option-value>1.2.3.4</dhcp-option-value>\
            </dhcp-option>\
            <dhcp-option>\
                <dhcp-option-name>15</dhcp-option-name>\
                <dhcp-option-value>test.com</dhcp-option-value>\
            </dhcp-option>\
        </dhcp-option-list>\
    </network-ipam-mgmt>";

    char add_subnet_tags[] =
    "<dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>6</dhcp-option-name>\
            <dhcp-option-value>0.0.0.0</dhcp-option-value>\
        </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>4</dhcp-option-name>\
            <dhcp-option-value>13.12.14.15</dhcp-option-value>\
        </dhcp-option>\
     </dhcp-option-list>";

    CreateVmportEnv(input, 2, 0, NULL, NULL);
    client->WaitForIdle();
    client->Reset();
    AddIPAM("vn1", ipam_info, 3, ipam_attr, "vdns1", NULL, add_subnet_tags);
    client->WaitForIdle();

    ClearPktTrace();
    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 5, input[0].mac);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 5, input[0].mac);
    int count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

    #define ZERO_DNS_SUBNET_OPTION "Server : 1.1.1.200; Subnet mask : 255.255.255.0; Broadcast : 1.1.1.255; Time Server : 13.12.14.15; Domain Name : test.com; Lease time : 4294967295; Gateway : 1.1.1.200; Host Name : vm1; "
    DhcpInfo *sand = new DhcpInfo();
    Sandesh::set_response_callback(boost::bind(&DhcpTest::CheckSandeshResponse,
                                               this, _1, true, "",
                                               ZERO_DNS_SUBNET_OPTION,
                                               false, "", false));
    sand->HandleRequest();
    client->WaitForIdle();
    sand->Release();

    client->Reset();
    DelIPAM("vn1", "vdns1");
    client->WaitForIdle();
    DelVDNS("vdns1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 2, 1, 0);
    client->WaitForIdle();

    ClearPktTrace();
    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}

// Check dhcp options - router option when configured
TEST_F(DhcpTest, RouterOption) {
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>routers</dhcp-option-name>\
            <dhcp-option-value>12.13.14.15 23.24.25.26</dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    #define OPTION_CATEGORY_ROUTER "Gateway : 12.13.14.15; Gateway : 23.24.25.26;"
    DhcpOptionCategoryTest(vm_interface_attr, true, OPTION_CATEGORY_ROUTER,
                           false, "");
}

// Check dhcp options - dhcp options sent by Ironic openstack
TEST_F(DhcpTest, IronicDhcpOptions) {
    char vm_interface_attr[] =
        "<virtual-machine-interface-dhcp-option-list>\
            <dhcp-option>\
                <dhcp-option-name>tag:ipxe,67</dhcp-option-name>\
                <dhcp-option-value>TEST</dhcp-option-value>\
            </dhcp-option>\
        </virtual-machine-interface-dhcp-option-list>";

      // check size of option value in dhcp response to verify option is set
      #define OPTION_CATEGORY_BOOTFILE_NAME "43 04 54 45 53 54 "
      DhcpOptionCategoryTest(vm_interface_attr, false, " ", true, OPTION_CATEGORY_BOOTFILE_NAME);
}

// Check dhcp options - classless host routes
TEST_F(DhcpTest, ClasslessOption) {
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>classless-static-routes</dhcp-option-name>\
            <dhcp-option-value>10.1.2.0/24 0.0.0.0 20.20.20.0/24 20.20.20.1</dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    #define OPTION_CATEGORY_CLASSLESS "Host Routes : 10.1.2.0/24 -> 1.1.1.200;20.20.20.0/24 -> 20.20.20.1;"
    DhcpOptionCategoryTest(vm_interface_attr, true, OPTION_CATEGORY_CLASSLESS,
                           false, "");
}

// Check dhcp options - different categories
TEST_F(DhcpTest, ClasslessOptionError) {
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>classless-static-routes</dhcp-option-name>\
            <dhcp-option-value>10.1.2.0/24 0.0.0.0 abcd</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>classless-static-routes</dhcp-option-name>\
            <dhcp-option-value>20.20.20.0/24 20.20.20.1 abcd</dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    // second option is not added as it is repeated
    #define OPTION_CATEGORY_CLASSLESS_ERROR "Host Routes : 10.1.2.0/24 -> 1.1.1.200;"
    DhcpOptionCategoryTest(vm_interface_attr, true,
                           OPTION_CATEGORY_CLASSLESS_ERROR, false, "");
}

// Check dhcp options - name compression following DNS name encoding rules
TEST_F(DhcpTest, NameCompressionOption) {
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>domain-search</dhcp-option-name>\
            <dhcp-option-value>test.juniper.net</dhcp-option-value>\
         </dhcp-option>\
        <dhcp-option>\
            <dhcp-option-name>domain-search</dhcp-option-name>\
            <dhcp-option-value></dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    #define OPTION_CATEGORY_COMPRESSED_NAME "77 12 04 74 65 73 74 07 6a 75 6e 69 70 65 72 03 6e 65 74 00 "
    DhcpOptionCategoryTest(vm_interface_attr, false, "",
                           true, OPTION_CATEGORY_COMPRESSED_NAME);
}

// Check dhcp options - lease value
TEST_F(DhcpTest, LeaseValueOption) {
    char vm_interface_attr[] =
    "<virtual-machine-interface-dhcp-option-list>\
        <dhcp-option>\
            <dhcp-option-name>dhcp-lease-time</dhcp-option-name>\
            <dhcp-option-value>1500</dhcp-option-value>\
         </dhcp-option>\
     </virtual-machine-interface-dhcp-option-list>";

    DhcpOptionCategoryTest(vm_interface_attr, true, "Lease time : 1500;", false, "");
}

// Check DHCP lease allocation & release for GW interface
TEST_F(DhcpTest, GatewayDhcpLeaseBasic) {
    struct PortInfo input[] = {
        {"vnet1", 1, "7.8.9.3", "00:00:00:01:01:01", 1, 1},
    };

    IpamInfo ipam_info[] = {
        {"1.2.3.0", 24, "1.2.3.1", true},
        {"7.8.9.0", 24, "7.8.9.1", true},
    };

    client->Reset();
    CreateVmportWithoutNova(input, 1);
    client->WaitForIdle();

    AddIPAM("vn1", ipam_info, 2);
    client->WaitForIdle();

    AddPhysicalDevice(Agent::GetInstance()->host_name().c_str(), 1);
    AddPhysicalInterface("physical1", 1, "physical1");
    AddLink("physical-router", Agent::GetInstance()->host_name().c_str(),
            "physical-interface", "physical1");
    AddLogicalInterface("logical1", 1, "logical1", 1);
    AddLink("logical-interface", "logical1", "physical-interface", "physical1");
    AddLink("virtual-machine-interface", "vnet1", "logical-interface", "logical1");

    //Add a link to interface subnet and ensure resolve route is added
    AddSubnetType("subnet", 1, "7.8.9.0", 24);
    AddLink("virtual-machine-interface", input[0].name, "subnet", "subnet");
    client->WaitForIdle();
    EXPECT_TRUE(VmPortActive(input, 0));
    EXPECT_TRUE(RouteFind("vrf1", "7.8.9.0", 24));

    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    // get one address allocated - 7.8.9.2
    ClearPktTrace();
    SendDhcp(GetGwItfId(), 0x8000, DHCP_DISCOVER, options, 3, input[0].mac);
    SendDhcp(GetGwItfId(), 0x8000, DHCP_REQUEST, options, 3, input[0].mac);
    int count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

#define GW_LEASE_OPTIONS_STRING "Ack; Server : 7.8.9.1; Subnet mask : 255.255.255.0; Broadcast : 7.8.9.255; Lease time : 86400; Gateway : 7.8.9.1; DNS : 7.8.9.1; "
    DhcpInfo *sand = new DhcpInfo();
    Sandesh::set_response_callback(boost::bind(&DhcpTest::CheckSandeshResponse,
                                               this, _1, true, "",
                                               GW_LEASE_OPTIONS_STRING,
                                               false, "", false, "7.8.9.2"));
    sand->HandleRequest();
    client->WaitForIdle();
    sand->Release();

    boost::system::error_code ec;
    LoadDhcpLeaseFile(Ip4Address::from_string("7.8.9.0", ec), 24,
                      "./dhcp.00000000-0000-0000-0000-000000000001.leases");
    EXPECT_TRUE(CheckDhcpLease(MacAddress(std::string(input[0].mac)),
                               Ip4Address::from_string("7.8.9.2", ec), false));

    ClearPktTrace();
    SendDhcp(GetGwItfId(), 0x8000, DHCP_RELEASE, options, 3, input[0].mac);
    count = 0;
    client->WaitForIdle();
    DHCP_CHECK (stats.release < 1);
    client->WaitForIdle();

    LoadDhcpLeaseFile(Ip4Address::from_string("7.8.9.0", ec), 24,
                      "./dhcp.00000000-0000-0000-0000-000000000001.leases");
    EXPECT_TRUE(CheckDhcpLease(MacAddress(std::string(input[0].mac)),
                               Ip4Address::from_string("7.8.9.2", ec), true));
    CloseDhcpLeaseFile();

    client->Reset();
    DelLink("virtual-machine-interface", input[0].name, "subnet", "subnet");
    DelLink("virtual-machine-interface", "vnet1", "logical-interface", "logical1");
    DelLink("logical-interface", "logical1", "physical-interface", "physical1");
    DelLink("physical-router", Agent::GetInstance()->host_name().c_str(),
            "physical-interface", "physical1");
    DeleteLogicalInterface("logical1");
    DeletePhysicalInterface("physical1");
    DeletePhysicalDevice(Agent::GetInstance()->host_name().c_str());

    client->Reset();
    DelIPAM("vn1");
    client->WaitForIdle();
    EXPECT_FALSE(RouteFind("vrf1", "7.8.9.0", 24));

    DeleteVmportEnv(input, 1, true);
    client->WaitForIdle();

    EXPECT_FALSE(VmPortFind(1));
    VmInterfaceKey key(AgentKey::ADD_DEL_CHANGE, MakeUuid(1), "");
    WAIT_FOR(100, 1000, (Agent::GetInstance()->interface_table()->Find(&key, true)
                == NULL));
    client->Reset();

    ClearPktTrace();
    Agent::GetInstance()->GetDhcpProto()->ClearStats();
    remove("./dhcp.00000000-0000-0000-0000-000000000001.leases");
}

TEST_F(DhcpTest, GatewayDhcpLeaseBasicVpg) {
    struct PortInfo input[] = {
        {"vnet1", 1, "7.8.9.3", "00:00:00:01:01:01", 1, 1},
    };

    IpamInfo ipam_info[] = {
        {"1.2.3.0", 24, "1.2.3.1", true},
        {"7.8.9.0", 24, "7.8.9.1", true},
    };

    client->Reset();
    CreateVmportWithoutNova(input, 1);
    client->WaitForIdle();

    AddIPAM("vn1", ipam_info, 2);
    client->WaitForIdle();

    AddPhysicalDevice(Agent::GetInstance()->host_name().c_str(), 1);
    AddPhysicalInterface("physical1", 1, "physical1");
    AddVirtualPortGroup("vpg1", 1, "vpg1");
    AddLinkNode("virtual-port-group-physical-interface",
                "vpg1_physical1", "ae1");
    AddLink("physical-router", Agent::GetInstance()->host_name().c_str(),
            "physical-interface", "physical1");
    AddLink("virtual-port-group-physical-interface", "vpg1_physical1",
            "physical-interface", "physical1",
            "virtual-port-group-physical-interface");
    AddLink("virtual-port-group-physical-interface", "vpg1_physical1",
            "virtual-port-group", "vpg1",
            "virtual-port-group-physical-interface");
    AddLink("virtual-machine-interface", "vnet1",
            "virtual-port-group", "vpg1");
    //Add a link to interface subnet and ensure resolve route is added
    AddSubnetType("subnet", 1, "7.8.9.0", 24);
    AddLink("virtual-machine-interface", input[0].name, "subnet", "subnet");
    client->WaitForIdle();
    EXPECT_TRUE(VmPortActive(input, 0));
    EXPECT_TRUE(RouteFind("vrf1", "7.8.9.0", 24));

    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    // get one address allocated - 7.8.9.2
    ClearPktTrace();
    SendDhcp(GetGwItfId(), 0x8000, DHCP_DISCOVER, options, 3, input[0].mac);
    SendDhcp(GetGwItfId(), 0x8000, DHCP_REQUEST, options, 3, input[0].mac);
    int count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

#define GW_LEASE_OPTIONS_STRING "Ack; Server : 7.8.9.1; Subnet mask : 255.255.255.0; Broadcast : 7.8.9.255; Lease time : 86400; Gateway : 7.8.9.1; DNS : 7.8.9.1; "
    DhcpInfo *sand = new DhcpInfo();
    Sandesh::set_response_callback(boost::bind(&DhcpTest::CheckSandeshResponse,
                                               this, _1, true, "",
                                               GW_LEASE_OPTIONS_STRING,
                                               false, "", false, "7.8.9.2"));
    sand->HandleRequest();
    client->WaitForIdle();
    sand->Release();

    boost::system::error_code ec;
    LoadDhcpLeaseFile(Ip4Address::from_string("7.8.9.0", ec), 24,
                      "./dhcp.00000000-0000-0000-0000-000000000001.leases");
    EXPECT_TRUE(CheckDhcpLease(MacAddress(std::string(input[0].mac)),
                               Ip4Address::from_string("7.8.9.2", ec), false));

    ClearPktTrace();
    SendDhcp(GetGwItfId(), 0x8000, DHCP_RELEASE, options, 3, input[0].mac);
    count = 0;
    client->WaitForIdle();
    DHCP_CHECK (stats.release < 1);
    client->WaitForIdle();

    LoadDhcpLeaseFile(Ip4Address::from_string("7.8.9.0", ec), 24,
                      "./dhcp.00000000-0000-0000-0000-000000000001.leases");
    EXPECT_TRUE(CheckDhcpLease(MacAddress(std::string(input[0].mac)),
                               Ip4Address::from_string("7.8.9.2", ec), true));
    CloseDhcpLeaseFile();

    client->Reset();
    DelLink("virtual-machine-interface", input[0].name, "subnet", "subnet");
    DelLink("virtual-machine-interface", "vnet1",
            "virtual-port-group", "vpg1");
    DelLink("physical-router", Agent::GetInstance()->host_name().c_str(),
            "physical-interface", "physical1");
    DelNode("virtual-port-group-physical-interface", "vpg1_physical1");
    DeleteVirtualPortGroup("vpg1");
    DeletePhysicalDevice(Agent::GetInstance()->host_name().c_str());
    DeletePhysicalInterface("physical1");

    client->Reset();
    DelIPAM("vn1");
    client->WaitForIdle();
    EXPECT_FALSE(RouteFind("vrf1", "7.8.9.0", 24));

    DeleteVmportEnv(input, 1, true);
    client->WaitForIdle();

    EXPECT_FALSE(VmPortFind(1));
    VmInterfaceKey key(AgentKey::ADD_DEL_CHANGE, MakeUuid(1), "");
    WAIT_FOR(100, 1000, (Agent::GetInstance()->
                            interface_table()->Find(&key, true) == NULL));
    client->Reset();

    ClearPktTrace();
    Agent::GetInstance()->GetDhcpProto()->ClearStats();
    remove("./dhcp.00000000-0000-0000-0000-000000000001.leases");
}

// Check MAX DHCP lease allocation
#ifdef DHCP_FLAKY
TEST_F(DhcpTest, GatewayDhcpLeaseMax) {
    struct PortInfo input[] = {
        {"vnet1", 1, "7.8.9.2", "00:00:00:01:01:01", 1, 1},
    };

    IpamInfo ipam_info[] = {
        {"1.2.3.0", 24, "1.2.3.1", true},
        {"7.8.9.0", 29, "7.8.9.1", true},
    };

    client->Reset();
    CreateVmportWithoutNova(input, 1);
    client->WaitForIdle();

    AddIPAM("vn1", ipam_info, 2);
    client->WaitForIdle();

    AddPhysicalDevice(Agent::GetInstance()->host_name().c_str(), 1);
    AddPhysicalInterface("physical1", 1, "physical1");
    AddLogicalInterface("logical1", 1, "logical1", 1);
    AddLink("physical-router", Agent::GetInstance()->host_name().c_str(),
            "physical-interface", "physical1");
    AddLink("logical-interface", "logical1", "physical-interface", "physical1");
    AddLink("virtual-machine-interface", "vnet1", "logical-interface", "logical1");

    AddSubnetType("subnet", 1, "7.8.9.0", 29);
    AddLink("virtual-machine-interface", input[0].name, "subnet", "subnet");
    client->WaitForIdle();

    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    // get all addresses allocated - 7.8.9.3 to 7.8.9.6
    std::string old_mac = src_mac.ToString();
    ClearPktTrace();
    src_mac = MacAddress::FromString("00:0a:0b:0c:0d:0a");
    SendDhcp(GetGwItfId(), 0x8000, DHCP_DISCOVER, options, 3);
    SendDhcp(GetGwItfId(), 0x8000, DHCP_REQUEST, options, 3);
    src_mac = MacAddress::FromString("00:0a:0b:0c:0d:0b");
    SendDhcp(GetGwItfId(), 0x8000, DHCP_DISCOVER, options, 3);
    SendDhcp(GetGwItfId(), 0x8000, DHCP_REQUEST, options, 3);
    src_mac = MacAddress::FromString("00:0a:0b:0c:0d:0c");
    SendDhcp(GetGwItfId(), 0x8000, DHCP_DISCOVER, options, 3);
    SendDhcp(GetGwItfId(), 0x8000, DHCP_REQUEST, options, 3);
    src_mac = MacAddress::FromString("00:0a:0b:0c:0d:0d");
    SendDhcp(GetGwItfId(), 0x8000, DHCP_DISCOVER, options, 3);
    SendDhcp(GetGwItfId(), 0x8000, DHCP_REQUEST, options, 3);
    int count = 0;
    DHCP_CHECK (stats.acks < 4);
    EXPECT_EQ(4U, stats.discover);
    EXPECT_EQ(4U, stats.request);
    EXPECT_EQ(4U, stats.offers);
    EXPECT_EQ(4U, stats.acks);
    client->WaitForIdle();

    boost::system::error_code ec;
    LoadDhcpLeaseFile(Ip4Address::from_string("7.8.9.0", ec), 29,
                      "./dhcp.00000000-0000-0000-0000-000000000001.leases");
    EXPECT_TRUE(CheckDhcpLease(MacAddress::FromString("00:0a:0b:0c:0d:0a"),
                               Ip4Address::from_string("7.8.9.3", ec),
                               false));
    EXPECT_TRUE(CheckDhcpLease(MacAddress::FromString("00:0a:0b:0c:0d:0b"),
                               Ip4Address::from_string("7.8.9.4", ec),
                               false));
    EXPECT_TRUE(CheckDhcpLease(MacAddress::FromString("00:0a:0b:0c:0d:0c"),
                               Ip4Address::from_string("7.8.9.5", ec),
                               false));
    EXPECT_TRUE(CheckDhcpLease(MacAddress::FromString("00:0a:0b:0c:0d:0d"),
                               Ip4Address::from_string("7.8.9.6", ec),
                               false));

    // leases from the subnet are done, request for a new address now
    src_mac = MacAddress::FromString("00:0a:0b:0c:0d:0e");
    SendDhcp(GetGwItfId(), 0x8000, DHCP_DISCOVER, options, 3);
    SendDhcp(GetGwItfId(), 0x8000, DHCP_REQUEST, options, 3);
    count = 0;
    DHCP_CHECK (stats.errors < 1);

    // release one mac
    src_mac = MacAddress::FromString("00:0a:0b:0c:0d:0c");
    SendDhcp(GetGwItfId(), 0x8000, DHCP_RELEASE, options, 3);
    count = 0;
    DHCP_CHECK (stats.release < 1);
    client->WaitForIdle();

    // check that released address is re-allocated when re-requested (same mac)
    ClearPktTrace();
    SendDhcp(GetGwItfId(), 0x8000, DHCP_DISCOVER, options, 3);
    SendDhcp(GetGwItfId(), 0x8000, DHCP_REQUEST, options, 3);
    count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

    // release another mac
    src_mac = MacAddress::FromString("00:0a:0b:0c:0d:0b");
    SendDhcp(GetGwItfId(), 0x8000, DHCP_RELEASE, options, 3);
    count = 0;
    DHCP_CHECK (stats.release < 1);
    client->WaitForIdle();

    // check that released address is re-allocated when re-requested (diff mac)
    ClearPktTrace();
    src_mac = MacAddress::FromString("00:0a:0b:0c:0d:0e");
    SendDhcp(GetGwItfId(), 0x8000, DHCP_DISCOVER, options, 3);
    SendDhcp(GetGwItfId(), 0x8000, DHCP_REQUEST, options, 3);
    count = 0;
    DHCP_CHECK (stats.acks < 1);
    EXPECT_EQ(1U, stats.discover);
    EXPECT_EQ(1U, stats.request);
    EXPECT_EQ(1U, stats.offers);
    EXPECT_EQ(1U, stats.acks);

    LoadDhcpLeaseFile(Ip4Address::from_string("7.8.9.0", ec), 29,
                      "./dhcp.00000000-0000-0000-0000-000000000001.leases");
    EXPECT_TRUE(CheckDhcpLease(MacAddress::FromString("00:0a:0b:0c:0d:0a"),
                               Ip4Address::from_string("7.8.9.3", ec),
                               false));
    EXPECT_TRUE(CheckDhcpLease(MacAddress::FromString("00:0a:0b:0c:0d:0e"),
                               Ip4Address::from_string("7.8.9.4", ec),
                               false));
    EXPECT_TRUE(CheckDhcpLease(MacAddress::FromString("00:0a:0b:0c:0d:0c"),
                               Ip4Address::from_string("7.8.9.5", ec),
                               false));
    EXPECT_TRUE(CheckDhcpLease(MacAddress::FromString("00:0a:0b:0c:0d:0d"),
                               Ip4Address::from_string("7.8.9.6", ec),
                               false));
    CloseDhcpLeaseFile();

    client->Reset();
    DelLink("virtual-machine-interface", input[0].name, "subnet", "subnet");
    DelLink("physical-router", Agent::GetInstance()->host_name().c_str(),
            "physical-interface", "physical1");
    DelLink("logical-interface", "logical1", "physical-interface", "physical1");
    DelLink("virtual-machine-interface", "vnet1", "logical-interface", "logical1");
    DeletePhysicalDevice(Agent::GetInstance()->host_name().c_str());
    DeletePhysicalInterface("physical1");
    DeleteLogicalInterface("logical1");

    client->Reset();
    DelIPAM("vn1");
    client->WaitForIdle();

    DeleteVmportEnv(input, 1, true);
    client->WaitForIdle();

    EXPECT_FALSE(VmPortFind(1));
    VmInterfaceKey key(AgentKey::ADD_DEL_CHANGE, MakeUuid(2), "");
    WAIT_FOR(100, 1000, (Agent::GetInstance()->interface_table()->Find(&key, true)
                == NULL));
    client->Reset();

    ClearPktTrace();
    Agent::GetInstance()->GetDhcpProto()->ClearStats();
    src_mac = MacAddress::FromString(old_mac);
    remove("./dhcp.00000000-0000-0000-0000-000000000001.leases");
}
#endif

static void CreateTestLeaseFile() {
    std::ofstream lease_ofstream;
    lease_ofstream.open("./dhcp.00000000-0000-0000-0000-000000000001.leases",
                        std::ofstream::out | std::ofstream::trunc);

    lease_ofstream << "\
<lease> <mac>00:0a:0b:0c:0d:0a</mac> <ip>7.8.9.3</ip> <expiry>0</expiry> <released>false</released> </lease>\n\
<lease> <mac>00:0a:0b:0c:0d:0c</mac> <ip>7.8.9.5</ip> <expiry>0</expiry> <released>false</released> </lease>\n\
<lease> <mac>00:0a:0b:0c:0d:0d</mac> <ip>7.8.9.6</ip> <expiry>0</expiry> <released>false</released> </lease>\n\
<lease> <mac>00:0a:0b:0c:0d:0e</mac> <ip>7.8.9.4</ip> <expiry>0</expiry> <released>false</released> </lease>\n\
";
    lease_ofstream.flush();
    lease_ofstream.close();
}

// Check DHCP lease allocation timeout
TEST_F(DhcpTest, GatewayDhcpLeaseTimeout) {
    CreateTestLeaseFile();

    struct PortInfo input[] = {
        {"vnet1", 1, "7.8.9.2", "00:00:00:01:01:01", 1, 1},
    };

    IpamInfo ipam_info[] = {
        {"1.2.3.0", 24, "1.2.3.1", true},
        {"7.8.9.0", 29, "7.8.9.1", true},
    };

    client->Reset();
    CreateVmportWithoutNova(input, 1);
    client->WaitForIdle();

    AddIPAM("vn1", ipam_info, 2);
    client->WaitForIdle();

    AddPhysicalDevice(Agent::GetInstance()->host_name().c_str(), 1);
    AddPhysicalInterface("physical1", 1, "physical1");
    AddLogicalInterface("logical1", 1, "logical1", 1);
    AddLink("physical-router", Agent::GetInstance()->host_name().c_str(),
            "physical-interface", "physical1");
    AddLink("logical-interface", "logical1", "physical-interface", "physical1");
    AddLink("virtual-machine-interface", "vnet1", "logical-interface", "logical1");

    AddSubnetType("subnet", 1, "7.8.9.0", 29);
    AddLink("virtual-machine-interface", input[0].name, "subnet", "subnet");
    client->WaitForIdle();

    int count = 0;
    boost::system::error_code ec;
    SetDhcpLeaseTimeout(10);
    client->WaitForIdle();
    do {
        count++;
        usleep(100000);
        LoadDhcpLeaseFile(Ip4Address::from_string("7.8.9.0", ec), 29,
                          "./dhcp.00000000-0000-0000-0000-000000000001.leases");
    } while(CheckDhcpLease(MacAddress::FromString("00:0a:0b:0c:0d:0a"),
                           Ip4Address::from_string("7.8.9.3", ec), true)
            == false && count < MAX_WAIT_COUNT);

    EXPECT_TRUE(CheckDhcpLease(MacAddress::FromString("00:0a:0b:0c:0d:0a"),
                               Ip4Address::from_string("7.8.9.3", ec),
                               true));
    EXPECT_TRUE(CheckDhcpLease(MacAddress::FromString("00:0a:0b:0c:0d:0e"),
                               Ip4Address::from_string("7.8.9.4", ec),
                               true));
    EXPECT_TRUE(CheckDhcpLease(MacAddress::FromString("00:0a:0b:0c:0d:0c"),
                               Ip4Address::from_string("7.8.9.5", ec),
                               true));
    EXPECT_TRUE(CheckDhcpLease(MacAddress::FromString("00:0a:0b:0c:0d:0d"),
                               Ip4Address::from_string("7.8.9.6", ec),
                               true));
    CloseDhcpLeaseFile();

    client->Reset();
    DelLink("virtual-machine-interface", input[0].name, "subnet", "subnet");
    DelLink("physical-router", Agent::GetInstance()->host_name().c_str(),
            "physical-interface", "physical1");
    DelLink("logical-interface", "logical1", "physical-interface", "physical1");
    DelLink("virtual-machine-interface", "vnet1", "logical-interface", "logical1");
    DeletePhysicalDevice(Agent::GetInstance()->host_name().c_str());
    DeletePhysicalInterface("physical1");
    DeleteLogicalInterface("logical1");

    client->Reset();
    DelIPAM("vn1");
    client->WaitForIdle();

    DeleteVmportEnv(input, 1, true);
    client->WaitForIdle();

    EXPECT_FALSE(VmPortFind(1));
    VmInterfaceKey key(AgentKey::ADD_DEL_CHANGE, MakeUuid(2), "");
    WAIT_FOR(100, 1000, (Agent::GetInstance()->interface_table()->Find(&key, true)
                == NULL));
    client->Reset();

    ClearPktTrace();
    Agent::GetInstance()->GetDhcpProto()->ClearStats();
    remove("./dhcp.00000000-0000-0000-0000-000000000001.leases");
}

// Send DHCP request to v6 port
TEST_F(DhcpTest, DhcpReqv6PortTest) {
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
    };
    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    ClearPktTrace();
    IpamInfo ipam_info[] = {
        {"1.1.1.0", 24, "1.1.1.200", true},
    };
    char vdns_attr[] = "<virtual-DNS-data>\n <domain-name>test.contrail.juniper.net</domain-name>\n <dynamic-records-from-client>true</dynamic-records-from-client>\n <record-order>fixed</record-order>\n <default-ttl-seconds>120</default-ttl-seconds>\n </virtual-DNS-data>\n";
    char ipam_attr[] = "<network-ipam-mgmt>\n <ipam-dns-method>virtual-dns-server</ipam-dns-method>\n <ipam-dns-server><virtual-dns-server-name>vdns1</virtual-dns-server-name></ipam-dns-server>\n </network-ipam-mgmt>\n";

    CreateVmportEnv(input, 1, 0);
    client->WaitForIdle();
    client->Reset();
    AddVDNS("vdns1", vdns_attr);
    client->WaitForIdle();
    AddIPAM("vn1", ipam_info, 1, ipam_attr, "vdns1");
    client->WaitForIdle();

    SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 4,
             input[0].mac, false, false, 0, 0, DHCPV6_SERVER_PORT);
    SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 4,
             input[0].mac, false, false, 0, 0, DHCPV6_SERVER_PORT);
    client->WaitForIdle();
    EXPECT_EQ(0U, stats.discover);
    EXPECT_EQ(0U, stats.request);
    EXPECT_EQ(0U, stats.offers);
    EXPECT_EQ(0U, stats.acks);

    client->Reset();
    DelIPAM("vn1", "vdns1");
    client->WaitForIdle();
    DelVDNS("vdns1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 1, 1, 0);
    client->WaitForIdle();

    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}

// Check the DHCP queue limit
#ifdef DHCP_FLAKY
TEST_F(DhcpTest, QueueLimitTest) {
    struct PortInfo input[] = {
        {"vnet1", 1, "1.1.1.1", "00:00:00:01:01:01", 1, 1},
    };

    uint8_t options[] = {
        DHCP_OPTION_MSG_TYPE,
        DHCP_OPTION_HOST_NAME,
        DHCP_OPTION_DOMAIN_NAME,
        DHCP_OPTION_END
    };
    DhcpProto::DhcpStats stats;

    ClearPktTrace();
    IpamInfo ipam_info[] = {
        {"1.2.3.128", 27, "1.2.3.129", true},
        {"7.8.9.0", 24, "7.8.9.12", true},
        {"1.1.1.0", 24, "1.1.1.200", true},
    };
    char vdns_attr[] = "<virtual-DNS-data>\n <domain-name>test.contrail.juniper.net</domain-name>\n <dynamic-records-from-client>true</dynamic-records-from-client>\n <record-order>fixed</record-order>\n <default-ttl-seconds>120</default-ttl-seconds>\n </virtual-DNS-data>\n";
    char ipam_attr[] = "<network-ipam-mgmt>\n <ipam-dns-method>virtual-dns-server</ipam-dns-method>\n <ipam-dns-server><virtual-dns-server-name>vdns1</virtual-dns-server-name></ipam-dns-server>\n </network-ipam-mgmt>\n";

    CreateVmportEnv(input, 1, 0);
    client->WaitForIdle();
    client->Reset();
    AddVDNS("vdns1", vdns_attr);
    client->WaitForIdle();
    AddIPAM("vn1", ipam_info, 3, ipam_attr, "vdns1");
    client->WaitForIdle();

    // disable pkt handler queue, enqueue packets and
    // check that limit is not exceeded
    WorkQueue<boost::shared_ptr<PktInfo> > *queue =
        const_cast<Proto::ProtoWorkQueue *>(
        Agent::GetInstance()->GetDhcpProto()->work_queue());
    queue->set_disable(true);
    EXPECT_EQ(queue->Length(), 0);
    for (int i = 0; i < 2048; i++) {
        SendDhcp(GetItfId(0), 0x8000, DHCP_DISCOVER, options, 4, input[0].mac);
        SendDhcp(GetItfId(0), 0x8000, DHCP_REQUEST, options, 4, input[0].mac);
    }
    EXPECT_EQ(queue->Length(), 1023);
    queue->set_disable(false);

    client->Reset();
    DelIPAM("vn1", "vdns1");
    client->WaitForIdle();
    DelVDNS("vdns1");
    client->WaitForIdle();

    client->Reset();
    DeleteVmportEnv(input, 1, 1, 0);
    client->WaitForIdle();

    Agent::GetInstance()->GetDhcpProto()->ClearStats();
}
#endif

void RouterIdDepInit(Agent *agent) {
}

int main(int argc, char *argv[]) {
    GETUSERARGS();

    client = TestInit(init_file, ksync_init, true, true);
    usleep(100000);
    client->WaitForIdle();

    int ret = RUN_ALL_TESTS();
    TestShutdown();
    delete client;
    return ret;
}
