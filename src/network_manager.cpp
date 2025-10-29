#include "network_manager.hpp"

#include "config_parser.hpp"
#include "ipaddress.hpp"
#include "system_queries.hpp"
#include "types.hpp"
#include "util.hpp"

#include <linux/if_addr.h>
#include <linux/neighbour.h>
#include <net/if.h>
#include <net/if_arp.h>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/message.hpp>
#include <stdplus/numeric/str.hpp>
#include <stdplus/pinned.hpp>
#include <stdplus/print.hpp>
#include <stdplus/str/cat.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <filesystem>
#include <format>
#include <fstream>

namespace phosphor
{
namespace network
{

using namespace phosphor::logging;
using namespace sdbusplus::xyz::openbmc_project::Common::Error;
using Argument = xyz::openbmc_project::Common::InvalidArgument;
using std::literals::string_view_literals::operator""sv;

constexpr auto systemdBusname = "org.freedesktop.systemd1";
constexpr auto systemdObjPath = "/org/freedesktop/systemd1";
constexpr auto systemdInterface = "org.freedesktop.systemd1.Manager";
constexpr auto lldpFilePath = "/etc/lldpd.conf";
constexpr auto lldpService = "lldpd.service";

static constexpr const char enabledMatch[] =
    "type='signal',sender='org.freedesktop.network1',path_namespace='/org/"
    "freedesktop/network1/"
    "link',interface='org.freedesktop.DBus.Properties',member='"
    "PropertiesChanged',arg0='org.freedesktop.network1.Link',";

// 构造函数接收四个关键参数
// bus：D-Bus 总线连接引用
// reload：延迟执行器引用，用于配置重载
// objPath：D-Bus 对象路径
// confDir：配置文件目录路径
Manager::Manager(stdplus::PinnedRef<sdbusplus::bus_t> bus,
                 stdplus::PinnedRef<DelayedExecutor> reload,
                 stdplus::zstring_view objPath,
                 const std::filesystem::path& confDir) :
    ManagerIface(bus, objPath.c_str(), ManagerIface::action::defer_emit),
    reload(reload), bus(bus), objPath(std::string(objPath)), confDir(confDir),

    // D - Bus 信号监听与系统状态同步
    // 这段代码设置了一个 D-Bus 信号匹配器，用于监听 systemd-network1
    // 服务中网络接口的 AdministrativeState 管理员状态，属性变化

    // 解析消息获取接口路径和状态值
    // 从路径中提取接口索引（ifidx） 调用
    // handleAdminState 方法处理状态变更
    // 包含异常处理机制，确保单个接口的错误不会影响整体功能
    systemdNetworkdEnabledMatch(
        bus, enabledMatch,
        [man = stdplus::PinnedRef(*this)](sdbusplus::message_t& m) {
            std::string intf;
            std::unordered_map<std::string, std::variant<std::string>> values;
            try
            {
                m.read(intf, values);
                auto it = values.find("AdministrativeState");
                if (it == values.end())
                {
                    return;
                }
                const std::string_view obj = m.get_path();
                auto sep = obj.rfind('/');
                if (sep == obj.npos || sep + 3 > obj.size())
                {
                    throw std::invalid_argument("Invalid obj path");
                }
                auto ifidx =
                    stdplus::StrToInt<10, uint16_t>{}(obj.substr(sep + 3));
                const auto& state = std::get<std::string>(it->second);
                man.get().handleAdminState(state, ifidx);
            }
            catch (const std::exception& e)
            {
                lg2::error("AdministrativeState match parsing failed: {ERROR}",
                           "ERROR", e);
            }
        })
{
    // 配置重载回调
    // 设置了延迟执行器的回调函数，该函数在定时器触发时执行
    // 执行所有注册的 reloadPreHooks（重载前钩子函数）
    // 通过 D-Bus 调用 systemd-network1 服务的 Reload 方法，重新加载网络配置
    // 执行所有注册的 reloadPostHooks（重载后钩子函数）
    // 每次执行后清理钩子函数列表
    reload.get().setCallback([self = stdplus::PinnedRef(*this)]() {
        for (auto& hook : self.get().reloadPreHooks)
        {
            try
            {
                hook();
            }
            catch (const std::exception& ex)
            {
                lg2::error("Failed executing reload hook, ignoring: {ERROR}",
                           "ERROR", ex);
            }
        }
        self.get().reloadPreHooks.clear();
        try
        {
            self.get()
                .bus.get()
                .new_method_call("org.freedesktop.network1",
                                 "/org/freedesktop/network1",
                                 "org.freedesktop.network1.Manager", "Reload")
                .call();
            lg2::info("Reloaded systemd-networkd");
        }
        catch (const sdbusplus::exception_t& ex)
        {
            lg2::error("Failed to reload configuration: {ERROR}", "ERROR", ex);
            self.get().reloadPostHooks.clear();
        }
        for (auto& hook : self.get().reloadPostHooks)
        {
            try
            {
                hook();
            }
            catch (const std::exception& ex)
            {
                lg2::error("Failed executing reload hook, ignoring: {ERROR}",
                           "ERROR", ex);
            }
        }
        self.get().reloadPostHooks.clear();
    });

    // 这段代码负责初始化时获取并处理所有当前网络接口的状态：
    // 通过 D-Bus调用，ListLinks方法获取所有网络接口列表 对每个接口，构造其
    //  D-Bus对象路径，调用Get方法获取接口的 AdministrativeState 属性
    // 调用handleAdminState 方法处理接口状态 忽略 systemd-networkd
    std::vector<
        std::tuple<int32_t, std::string, sdbusplus::message::object_path>>
        links;
    try
    {
        auto rsp = bus.get()
                       .new_method_call("org.freedesktop.network1",
                                        "/org/freedesktop/network1",
                                        "org.freedesktop.network1.Manager",
                                        "ListLinks")
                       .call();
        rsp.read(links);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        // Any failures are systemd-network not being ready
    }
    for (const auto& link : links)
    {
        unsigned ifidx = std::get<0>(link);
        stdplus::ToStrHandle<stdplus::IntToStr<10, unsigned>> tsh;
        auto obj =
            stdplus::strCat("/org/freedesktop/network1/link/_3"sv, tsh(ifidx));
        auto req =
            bus.get().new_method_call("org.freedesktop.network1", obj.c_str(),
                                      "org.freedesktop.DBus.Properties", "Get");
        req.append("org.freedesktop.network1.Link", "AdministrativeState");
        auto rsp = req.call();
        std::variant<std::string> val;
        rsp.read(val);
        handleAdminState(std::get<std::string>(val), ifidx);
    }

    // 系统配置初始化
    std::filesystem::create_directories(confDir);
    // 系统配置config
    // /xyz/openbmc_project/network
    systemConf = std::make_unique<phosphor::network::SystemConfiguration>(
        bus, (this->objPath / "config").str);
}

//  主要负责创建或更新以太网接口对象
//  该方法负责根据提供的网络接口信息（AllIntfInfo结构体）
//  创建新的以太网接口对象，或更新已存在的接口对象。它是网络管理器初始化和维护网络接口的关键环节
//  info:
//  包含了接口的完整信息，如接口基本信息、默认网关、IP地址、静态邻居和静态网关等。
//  enabled : 表示接口是否启用
void Manager::createInterface(const AllIntfInfo& info, bool enabled)
{
    if (ignoredIntf.find(info.intf.idx) != ignoredIntf.end())
    {
        return;
    }
    if (auto it = interfacesByIdx.find(info.intf.idx);
        it != interfacesByIdx.end())
    {
        if (info.intf.name && *info.intf.name != it->second->interfaceName())
        {
            interfaces.erase(it->second->interfaceName());
            interfacesByIdx.erase(it);
        }
        else
        {
            it->second->updateInfo(info.intf);
            return;
        }
    }
    else if (info.intf.name)
    {
        auto it = interfaces.find(*info.intf.name);
        if (it != interfaces.end())
        {
            it->second->updateInfo(info.intf);
            return;
        }
    }
    if (!info.intf.name)
    {
        lg2::error("Can't create interface without name: {NET_IDX}", "NET_IDX",
                   info.intf.idx);
        return;
    }
    // 解析该接口对应的配置文件
    // 创建 EthernetInterface
    // 对象，传入总线、管理器引用、接口信息、对象路径、配置和启用状态等参数
    config::Parser config(config::pathForIntfConf(confDir, *info.intf.name));
    auto intf = std::make_unique<EthernetInterface>(
        bus, *this, info, objPath.str, config, enabled);

    // 从配置文件中加载DNS服务器和NTP服务器设置
    intf->loadNameServers(config);
    intf->loadNTPServers(config);

    // 接口对象注册
    // 网络配置持久化与运行时状态管理之间的桥梁
    auto ptr = intf.get();
    interfaces.insert_or_assign(*info.intf.name, std::move(intf));
    interfacesByIdx.insert_or_assign(info.intf.idx, ptr);
}

// 负责根据接口信息决定是否创建和管理网络接口，并在系统中维护接口状态
void Manager::addInterface(const InterfaceInfo& info)
{
    // 接口类型过滤
    if (info.type != ARPHRD_ETHER)
    {
        ignoredIntf.emplace(info.idx);
        return;
    }
    // 接口名称过滤
    if (info.name)
    {
        const auto& ignored = internal::getIgnoredInterfaces();
        if (ignored.find(*info.name) != ignored.end())
        {
            static std::unordered_set<std::string> ignored;
            if (!ignored.contains(*info.name))
            {
                ignored.emplace(*info.name);
                lg2::info("Ignoring interface {NET_INTF}", "NET_INTF",
                          *info.name);
            }
            ignoredIntf.emplace(info.idx);
            return;
        }
    }

    // 接口信息更新或创建
    auto infoIt = intfInfo.find(info.idx);
    if (infoIt != intfInfo.end())
    {
        // 找到了接口信息，更新接口信息
        infoIt->second.intf = info;
    }
    else
    {
        // 未找到接口信息，创建新的接口信息
        infoIt = std::get<0>(intfInfo.emplace(info.idx, AllIntfInfo{info}));
    }

    // 接口创建决策
    if (auto it = systemdNetworkdEnabled.find(info.idx);
        it != systemdNetworkdEnabled.end())
    {
        // 该接口在 systemdNetworkdEnabled
        // 映射中，创建以太网接口对象，传入完整的接口信息和启用状态
        createInterface(infoIt->second, it->second);
    }
}

void Manager::removeInterface(const InterfaceInfo& info)
{
    auto iit = interfacesByIdx.find(info.idx);
    auto nit = interfaces.end();
    if (info.name)
    {
        nit = interfaces.find(*info.name);
        if (nit != interfaces.end() && iit != interfacesByIdx.end() &&
            nit->second.get() != iit->second)
        {
            stdplus::print(stderr, "Removed interface desync detected\n");
            fflush(stderr);
            std::abort();
        }
    }
    else if (iit != interfacesByIdx.end())
    {
        for (nit = interfaces.begin(); nit != interfaces.end(); ++nit)
        {
            if (nit->second.get() == iit->second)
            {
                break;
            }
        }
    }

    if (iit != interfacesByIdx.end())
    {
        interfacesByIdx.erase(iit);
    }
    else
    {
        ignoredIntf.erase(info.idx);
    }
    if (nit != interfaces.end())
    {
        interfaces.erase(nit);
    }
    intfInfo.erase(info.idx);
}

void Manager::addAddress(const AddressInfo& info)
{
    if (info.flags & IFA_F_DEPRECATED)
    {
        return;
    }
    if (auto it = intfInfo.find(info.ifidx); it != intfInfo.end())
    {
        it->second.addrs.insert_or_assign(info.ifaddr, info);
        if (auto it = interfacesByIdx.find(info.ifidx);
            it != interfacesByIdx.end())
        {
            it->second->addAddr(info);
        }
    }
    else if (!ignoredIntf.contains(info.ifidx))
    {
        throw std::runtime_error(
            std::format("Interface `{}` not found for addr", info.ifidx));
    }
}

void Manager::removeAddress(const AddressInfo& info)
{
    if (auto it = interfacesByIdx.find(info.ifidx); it != interfacesByIdx.end())
    {
        it->second->addrs.erase(info.ifaddr);
        if (auto it = intfInfo.find(info.ifidx); it != intfInfo.end())
        {
            it->second.addrs.erase(info.ifaddr);
        }
    }
}

void Manager::addNeighbor(const NeighborInfo& info)
{
    if (!(info.state & NUD_PERMANENT) || !info.addr)
    {
        return;
    }
    if (auto it = intfInfo.find(info.ifidx); it != intfInfo.end())
    {
        it->second.staticNeighs.insert_or_assign(*info.addr, info);
        if (auto it = interfacesByIdx.find(info.ifidx);
            it != interfacesByIdx.end())
        {
            it->second->addStaticNeigh(info);
        }
    }
    else if (!ignoredIntf.contains(info.ifidx))
    {
        throw std::runtime_error(
            std::format("Interface `{}` not found for neigh", info.ifidx));
    }
}

void Manager::removeNeighbor(const NeighborInfo& info)
{
    if (!info.addr)
    {
        return;
    }
    if (auto it = intfInfo.find(info.ifidx); it != intfInfo.end())
    {
        it->second.staticNeighs.erase(*info.addr);
        if (auto it = interfacesByIdx.find(info.ifidx);
            it != interfacesByIdx.end())
        {
            it->second->staticNeighbors.erase(*info.addr);
        }
    }
}

void Manager::addDefGw(unsigned ifidx, stdplus::InAnyAddr addr)
{
    if (auto it = intfInfo.find(ifidx); it != intfInfo.end())
    {
        std::visit(
            [&](auto addr) {
                if constexpr (std::is_same_v<stdplus::In4Addr, decltype(addr)>)
                {
                    it->second.defgw4.emplace(addr);
                }
                else
                {
                    static_assert(
                        std::is_same_v<stdplus::In6Addr, decltype(addr)>);
                    it->second.defgw6.emplace(addr);
                }
            },
            addr);
        if (auto it = interfacesByIdx.find(ifidx); it != interfacesByIdx.end())
        {
            std::visit(
                [&](auto addr) {
                    if constexpr (std::is_same_v<stdplus::In4Addr,
                                                 decltype(addr)>)
                    {
                        it->second->EthernetInterfaceIntf::defaultGateway(
                            stdplus::toStr(addr));
                    }
                    else
                    {
                        static_assert(
                            std::is_same_v<stdplus::In6Addr, decltype(addr)>);
                        it->second->EthernetInterfaceIntf::defaultGateway6(
                            stdplus::toStr(addr));
                    }
                },
                addr);
        }
    }
    else if (!ignoredIntf.contains(ifidx))
    {
        lg2::error("Interface {NET_IDX} not found for gw", "NET_IDX", ifidx);
    }
}

void Manager::removeDefGw(unsigned ifidx, stdplus::InAnyAddr addr)
{
    if (auto it = intfInfo.find(ifidx); it != intfInfo.end())
    {
        std::visit(
            [&](auto addr) {
                if constexpr (std::is_same_v<stdplus::In4Addr, decltype(addr)>)
                {
                    if (it->second.defgw4 == addr)
                    {
                        it->second.defgw4.reset();
                    }
                }
                else
                {
                    static_assert(
                        std::is_same_v<stdplus::In6Addr, decltype(addr)>);
                    if (it->second.defgw6 == addr)
                    {
                        it->second.defgw6.reset();
                    }
                }
            },
            addr);
        if (auto it = interfacesByIdx.find(ifidx); it != interfacesByIdx.end())
        {
            std::visit(
                [&](auto addr) {
                    if constexpr (std::is_same_v<stdplus::In4Addr,
                                                 decltype(addr)>)
                    {
                        stdplus::ToStrHandle<stdplus::ToStr<stdplus::In4Addr>>
                            tsh;
                        if (it->second->defaultGateway() == tsh(addr))
                        {
                            it->second->EthernetInterfaceIntf::defaultGateway(
                                "");
                        }
                    }
                    else
                    {
                        static_assert(
                            std::is_same_v<stdplus::In6Addr, decltype(addr)>);
                        stdplus::ToStrHandle<stdplus::ToStr<stdplus::In6Addr>>
                            tsh;
                        if (it->second->defaultGateway6() == tsh(addr))
                        {
                            it->second->EthernetInterfaceIntf::defaultGateway6(
                                "");
                        }
                    }
                },
                addr);
        }
    }
}

ObjectPath Manager::vlan(std::string interfaceName, uint32_t id)
{
    if (id == 0 || id >= 4095)
    {
        lg2::error("VLAN ID {NET_VLAN} is not valid", "NET_VLAN", id);
        elog<InvalidArgument>(
            Argument::ARGUMENT_NAME("VLANId"),
            Argument::ARGUMENT_VALUE(std::to_string(id).c_str()));
    }

    auto it = interfaces.find(interfaceName);
    if (it == interfaces.end())
    {
        using ResourceErr =
            phosphor::logging::xyz::openbmc_project::Common::ResourceNotFound;
        elog<ResourceNotFound>(ResourceErr::RESOURCE(interfaceName.c_str()));
    }
    return it->second->createVLAN(id);
}

void Manager::reset()
{
    for (const auto& dirent : std::filesystem::directory_iterator(confDir))
    {
        std::error_code ec;
        std::filesystem::remove(dirent.path(), ec);
    }
    lg2::info("Network data purged.");
}

void Manager::writeToConfigurationFile()
{
    // write all the static ip address in the systemd-network conf file
    for (const auto& intf : interfaces)
    {
        intf.second->writeConfigurationFile();
    }
}

// 接收网络接口的管理状态字符串和接口索引，根据不同的状态值执行相应的操作，主要用于维护
// systemd-networkd 与 phosphor-networkd 之间的接口管理状态同步
void Manager::handleAdminState(std::string_view state, unsigned ifidx)
{
    if (state == "initialized" || state == "linger")
    {
        systemdNetworkdEnabled.erase(ifidx);
    }
    else
    {
        bool managed = state != "unmanaged";
        systemdNetworkdEnabled.insert_or_assign(ifidx, managed);
        if (auto it = intfInfo.find(ifidx); it != intfInfo.end())
        {
            if (exist config)
            {
                // 修改it->second == AllIntfInfo，根据配置文件
                // --- 根据配置文件创建接口 ---
                AllIntfInfo& tmp = it->second;
                tmp.inf
                
                createInterface(it->second, managed);
                // 写配置重新加载
                // 如果存在配置文件
                writeConfigurationFile();
                reloadConfigs();
            }
            else
            {
                createInterface(it->second, managed);
            }
        }
    }
}

void Manager::writeLLDPDConfigurationFile()
{
    std::ofstream lldpdConfig(lldpFilePath);

    lldpdConfig << "configure system description BMC" << std::endl;
    lldpdConfig << "configure system ip management pattern eth*" << std::endl;
    for (const auto& intf : interfaces)
    {
        bool emitlldp = intf.second->emitLLDP();
        if (emitlldp)
        {
            lldpdConfig << "configure ports " << intf.second->interfaceName()
                        << " lldp status tx-only" << std::endl;
        }
        else
        {
            lldpdConfig << "configure ports " << intf.second->interfaceName()
                        << " lldp status disabled" << std::endl;
        }
    }

    lldpdConfig.close();
}

void Manager::reloadLLDPService()
{
    try
    {
        auto method = bus.get().new_method_call(
            systemdBusname, systemdObjPath, systemdInterface, "RestartUnit");
        method.append(lldpService, "replace");
        bus.get().call_noreply(method);
    }
    catch (const sdbusplus::exception_t& ex)
    {
        lg2::error("Failed to restart service {SERVICE}: {ERR}", "SERVICE",
                   lldpService, "ERR", ex);
    }
}

} // namespace network
} // namespace phosphor
