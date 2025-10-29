#include "rtnetlink_server.hpp"

#include "netlink.hpp"
#include "network_manager.hpp"
#include "rtnetlink.hpp"

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <phosphor-logging/lg2.hpp>
#include <stdplus/fd/create.hpp>
#include <stdplus/fd/ops.hpp>

namespace phosphor::network::netlink
{

inline void rthandler(std::string_view data, auto&& cb)
{
    auto ret = gatewayFromRtm(data);
    if (!ret)
    {
        return;
    }
    cb(std::get<unsigned>(*ret), std::get<stdplus::InAnyAddr>(*ret));
}

static unsigned getIfIdx(const nlmsghdr& hdr, std::string_view data)
{
    switch (hdr.nlmsg_type)
    {
        case RTM_NEWLINK:
        case RTM_DELLINK:
            return extractRtData<ifinfomsg>(data).ifi_index;
        case RTM_NEWADDR:
        case RTM_DELADDR:
            return extractRtData<ifaddrmsg>(data).ifa_index;
        case RTM_NEWNEIGH:
        case RTM_DELNEIGH:
            return extractRtData<ndmsg>(data).ndm_ifindex;
    }
    throw std::runtime_error("Unknown nlmsg_type");
}

// 负责处理 Linux 内核通过 Netlink 协议发送的网络事件通知的核心处理函数
// 该函数是一个静态回调函数，作为内核网络事件的处理器，接收并解析来自 Linux
// 内核的 RTNETLINK 消息，然后将这些消息转换为 phosphor-networkd
// 网络管理系统中的相应操作，它是系统感知和响应底层网络变化的关键组件
static void handler(Manager& m, const nlmsghdr& hdr, std::string_view data)
{
    // 事件分发处理
    // 函数使用 switch 语句根据消息类型 (hdr.nlmsg_type)
    // intfFromRtm 解析接口信息
    // rthandler 辅助函数提取网关信息
    // neighFromRtm 解析邻居信息并转发给Manager相应方法
    try
    {
        switch (hdr.nlmsg_type)
        {
            case RTM_NEWLINK:
                m.addInterface(intfFromRtm(data));
                break;
            case RTM_DELLINK:
                m.removeInterface(intfFromRtm(data));
                break;
            case RTM_NEWROUTE:
                rthandler(data, [&](auto ifidx, auto addr) {
                    m.addDefGw(ifidx, addr);
                });
                break;
            case RTM_DELROUTE:
                rthandler(data, [&](auto ifidx, auto addr) {
                    m.removeDefGw(ifidx, addr);
                });
                break;
            case RTM_NEWADDR:
                m.addAddress(addrFromRtm(data));
                break;
            case RTM_DELADDR:
                m.removeAddress(addrFromRtm(data));
                break;
            case RTM_NEWNEIGH:
                m.addNeighbor(neighFromRtm(data));
                break;
            case RTM_DELNEIGH:
                m.removeNeighbor(neighFromRtm(data));
                break;
        }
    }
    catch (const std::exception& e)
    {
        try
        {
            if (m.ignoredIntf.contains(getIfIdx(hdr, data)))
            {
                // We don't want to log errors for ignored interfaces
                return;
            }
        }
        catch (...)
        {}
        lg2::error("Failed handling netlink event: {ERROR}", "ERROR", e);
    }
}

// 接收内核事件，把ip，等消息传递给handler处理
// 该函数是一个事件处理回调，当 Netlink 套接字上有数据可读时被调用
// 它负责从套接字接收数据，解析 Netlink 消息，并将这些消息传递给
// 主处理函数 handler 进行处理
static void eventHandler(Manager& m, sdeventplus::source::IO&, int fd, uint32_t)
{
    auto cb = [&](auto&&... args) {
        return handler(m, std::forward<decltype(args)>(args)...);
    };
    while (receive(fd, cb) > 0)
        ;
}

static stdplus::ManagedFd makeSock()
{
    using namespace stdplus::fd;

    auto sock = socket(SocketDomain::Netlink, SocketType::Raw,
                       static_cast<stdplus::fd::SocketProto>(NETLINK_ROUTE));

    sock.fcntlSetfl(sock.fcntlGetfl().set(FileFlag::NonBlock));

    sockaddr_nl local{};
    local.nl_family = AF_NETLINK;
    local.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR |
                      RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE | RTMGRP_NEIGH;
    bind(sock, local);

    return sock;
}

Server::Server(sdeventplus::Event& event, Manager& manager) :
    sock(makeSock()),
    io(event, sock.get(), EPOLLIN | EPOLLET, [&](auto&&... args) {
        return eventHandler(manager, std::forward<decltype(args)>(args)...);
    })
{
    auto cb = [&](const nlmsghdr& hdr, std::string_view data) {
        handler(manager, hdr, data);
    };
    performRequest(NETLINK_ROUTE, RTM_GETLINK, NLM_F_DUMP, ifinfomsg{}, cb);
    performRequest(NETLINK_ROUTE, RTM_GETADDR, NLM_F_DUMP, ifaddrmsg{}, cb);
    performRequest(NETLINK_ROUTE, RTM_GETROUTE, NLM_F_DUMP, rtmsg{}, cb);
    performRequest(NETLINK_ROUTE, RTM_GETNEIGH, NLM_F_DUMP, ndmsg{}, cb);
}

} // namespace phosphor::network::netlink
