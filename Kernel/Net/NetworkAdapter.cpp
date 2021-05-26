/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/Singleton.h>
#include <AK/StringBuilder.h>
#include <Kernel/Heap/kmalloc.h>
#include <Kernel/Lock.h>
#include <Kernel/Net/EtherType.h>
#include <Kernel/Net/EthernetFrameHeader.h>
#include <Kernel/Net/LoopbackAdapter.h>
#include <Kernel/Net/NetworkAdapter.h>
#include <Kernel/Process.h>
#include <Kernel/Random.h>
#include <Kernel/StdLib.h>

namespace Kernel {

static AK::Singleton<Lockable<HashTable<NetworkAdapter*>>> s_table;

Lockable<HashTable<NetworkAdapter*>>& NetworkAdapter::all_adapters()
{
    return *s_table;
}

RefPtr<NetworkAdapter> NetworkAdapter::from_ipv4_address(const IPv4Address& address)
{
    Locker locker(all_adapters().lock());
    for (auto* adapter : all_adapters().resource()) {
        if (adapter->ipv4_address() == address || adapter->ipv4_broadcast() == address)
            return adapter;
    }
    if (address[0] == 0 && address[1] == 0 && address[2] == 0 && address[3] == 0)
        return LoopbackAdapter::the();
    if (address[0] == 127)
        return LoopbackAdapter::the();
    return nullptr;
}

RefPtr<NetworkAdapter> NetworkAdapter::lookup_by_name(const StringView& name)
{
    NetworkAdapter* found_adapter = nullptr;
    for_each([&](auto& adapter) {
        if (adapter.name() == name)
            found_adapter = &adapter;
    });
    return found_adapter;
}

NetworkAdapter::NetworkAdapter()
{
    // FIXME: I wanna lock :(
    all_adapters().resource().set(this);
}

NetworkAdapter::~NetworkAdapter()
{
    // FIXME: I wanna lock :(
    all_adapters().resource().remove(this);
}

void NetworkAdapter::send(const MACAddress& destination, const ARPPacket& packet)
{
    size_t size_in_bytes = sizeof(EthernetFrameHeader) + sizeof(ARPPacket);
    auto buffer = NetworkByteBuffer::create_zeroed(size_in_bytes);
    auto* eth = (EthernetFrameHeader*)buffer.data();
    eth->set_source(mac_address());
    eth->set_destination(destination);
    eth->set_ether_type(EtherType::ARP);
    m_packets_out++;
    m_bytes_out += size_in_bytes;
    memcpy(eth->payload(), &packet, sizeof(ARPPacket));
    send_raw({ (const u8*)eth, size_in_bytes });
}

KResult NetworkAdapter::send_ipv4(const IPv4Address& source_ipv4, const MACAddress& destination_mac, const IPv4Address& destination_ipv4, IPv4Protocol protocol, const UserOrKernelBuffer& payload, size_t payload_size, u8 ttl)
{
    size_t ipv4_packet_size = sizeof(IPv4Packet) + payload_size;
    if (ipv4_packet_size > mtu())
        return send_ipv4_fragmented(source_ipv4, destination_mac, destination_ipv4, protocol, payload, payload_size, ttl);

    size_t ethernet_frame_size = sizeof(EthernetFrameHeader) + sizeof(IPv4Packet) + payload_size;
    auto buffer = NetworkByteBuffer::create_zeroed(ethernet_frame_size);
    auto& eth = *(EthernetFrameHeader*)buffer.data();
    eth.set_source(mac_address());
    eth.set_destination(destination_mac);
    eth.set_ether_type(EtherType::IPv4);
    auto& ipv4 = *(IPv4Packet*)eth.payload();
    ipv4.set_version(4);
    ipv4.set_internet_header_length(5);
    ipv4.set_source(source_ipv4);
    ipv4.set_destination(destination_ipv4);
    ipv4.set_protocol((u8)protocol);
    ipv4.set_length(sizeof(IPv4Packet) + payload_size);
    ipv4.set_ident(1);
    ipv4.set_ttl(ttl);
    ipv4.set_checksum(ipv4.compute_checksum());
    m_packets_out++;
    m_bytes_out += ethernet_frame_size;

    if (!payload.read(ipv4.payload(), payload_size))
        return EFAULT;
    send_raw({ (const u8*)&eth, ethernet_frame_size });
    return KSuccess;
}

KResult NetworkAdapter::send_ipv4_fragmented(const IPv4Address& source_ipv4, const MACAddress& destination_mac, const IPv4Address& destination_ipv4, IPv4Protocol protocol, const UserOrKernelBuffer& payload, size_t payload_size, u8 ttl)
{
    // packets must be split on the 64-bit boundary
    auto packet_boundary_size = (mtu() - sizeof(IPv4Packet) - sizeof(EthernetFrameHeader)) & 0xfffffff8;
    auto fragment_block_count = (payload_size + packet_boundary_size) / packet_boundary_size;
    auto last_block_size = payload_size - packet_boundary_size * (fragment_block_count - 1);
    auto number_of_blocks_in_fragment = packet_boundary_size / 8;

    auto identification = get_good_random<u16>();

    size_t ethernet_frame_size = mtu();
    for (size_t packet_index = 0; packet_index < fragment_block_count; ++packet_index) {
        auto is_last_block = packet_index + 1 == fragment_block_count;
        auto packet_payload_size = is_last_block ? last_block_size : packet_boundary_size;
        auto buffer = NetworkByteBuffer::create_zeroed(ethernet_frame_size);
        auto& eth = *(EthernetFrameHeader*)buffer.data();
        eth.set_source(mac_address());
        eth.set_destination(destination_mac);
        eth.set_ether_type(EtherType::IPv4);
        auto& ipv4 = *(IPv4Packet*)eth.payload();
        ipv4.set_version(4);
        ipv4.set_internet_header_length(5);
        ipv4.set_source(source_ipv4);
        ipv4.set_destination(destination_ipv4);
        ipv4.set_protocol((u8)protocol);
        ipv4.set_length(sizeof(IPv4Packet) + packet_payload_size);
        ipv4.set_has_more_fragments(!is_last_block);
        ipv4.set_ident(identification);
        ipv4.set_ttl(ttl);
        ipv4.set_fragment_offset(packet_index * number_of_blocks_in_fragment);
        ipv4.set_checksum(ipv4.compute_checksum());
        m_packets_out++;
        m_bytes_out += ethernet_frame_size;
        if (!payload.read(ipv4.payload(), packet_index * packet_boundary_size, packet_payload_size))
            return EFAULT;
        send_raw({ (const u8*)&eth, ethernet_frame_size });
    }
    return KSuccess;
}

void NetworkAdapter::did_receive(ReadonlyBytes payload)
{
    InterruptDisabler disabler;
    m_packets_in++;
    m_bytes_in += payload.size();

    if (m_packet_queue_size == max_packet_buffers) {
        // FIXME: Keep track of the number of dropped packets
        return;
    }

    auto packet = acquire_packet_buffer(payload.size());
    if (!packet) {
        dbgln("Discarding packet because we're out of memory");
        return;
    }

    memcpy(packet->buffer.data(), payload.data(), payload.size());

    m_packet_queue.append(*packet);
    m_packet_queue_size++;

    if (on_receive)
        on_receive();
}

size_t NetworkAdapter::dequeue_packet(u8* buffer, size_t buffer_size, Time& packet_timestamp)
{
    InterruptDisabler disabler;
    if (m_packet_queue.is_empty())
        return 0;
    auto packet_with_timestamp = m_packet_queue.take_first();
    m_packet_queue_size--;
    packet_timestamp = packet_with_timestamp->timestamp;
    auto& packet_buffer = packet_with_timestamp->buffer;
    size_t packet_size = packet_buffer.size();
    VERIFY(packet_size <= buffer_size);
    memcpy(buffer, packet_buffer.data(), packet_size);
    release_packet_buffer(*packet_with_timestamp);
    return packet_size;
}

RefPtr<PacketWithTimestamp> NetworkAdapter::acquire_packet_buffer(size_t size)
{
    InterruptDisabler disabler;
    if (m_unused_packets.is_empty()) {
        auto buffer = KBuffer::create_with_size(size, Region::Access::Read | Region::Access::Write, "Packet Buffer", AllocationStrategy::AllocateNow);
        auto packet = adopt_ref_if_nonnull(new PacketWithTimestamp { move(buffer), kgettimeofday() });
        packet->buffer.set_size(size);
        return packet;
    }

    auto packet = m_unused_packets.take_first();
    if (packet->buffer.capacity() >= size) {
        packet->timestamp = kgettimeofday();
        packet->buffer.set_size(size);
        return packet;
    }

    auto buffer = KBuffer::create_with_size(size, Region::Access::Read | Region::Access::Write, "Packet Buffer", AllocationStrategy::AllocateNow);
    packet = adopt_ref_if_nonnull(new PacketWithTimestamp { move(buffer), kgettimeofday() });
    packet->buffer.set_size(size);
    return packet;
}

void NetworkAdapter::release_packet_buffer(PacketWithTimestamp& packet)
{
    InterruptDisabler disabler;
    m_unused_packets.append(packet);
}

void NetworkAdapter::set_ipv4_address(const IPv4Address& address)
{
    m_ipv4_address = address;
}

void NetworkAdapter::set_ipv4_netmask(const IPv4Address& netmask)
{
    m_ipv4_netmask = netmask;
}

void NetworkAdapter::set_ipv4_gateway(const IPv4Address& gateway)
{
    m_ipv4_gateway = gateway;
}

void NetworkAdapter::set_interface_name(const PCI::Address& pci_address)
{
    // Note: This stands for e - "Ethernet", p - "Port" as for PCI bus, "s" for slot as for PCI slot
    auto name = String::formatted("ep{}s{}", pci_address.bus(), pci_address.device());
    VERIFY(!lookup_by_name(name));
    m_name = move(name);
}

void NetworkAdapter::set_loopback_name()
{
    auto name = String("loop");
    VERIFY(!lookup_by_name(name));
    m_name = move(name);
}
}
