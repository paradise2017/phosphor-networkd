#include "config.h"

#ifdef SYNC_MAC_FROM_INVENTORY
#include "inventory_mac.hpp"
#endif
#include "network_manager.hpp"
#include "rtnetlink_server.hpp"
#include "types.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/manager.hpp>
#include <sdeventplus/clock.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/signal.hpp>
#include <sdeventplus/utility/sdbus.hpp>
#include <sdeventplus/utility/timer.hpp>
#include <stdplus/pinned.hpp>
#include <stdplus/print.hpp>
#include <stdplus/signal.hpp>

#include <chrono>

constexpr char DEFAULT_OBJPATH[] = "/xyz/openbmc_project/network";

namespace phosphor::network
{

class TimerExecutor : public DelayedExecutor
{
  private:
    using Timer = sdeventplus::utility::Timer<sdeventplus::ClockId::Monotonic>;

  public:
    TimerExecutor(sdeventplus::Event& event, std::chrono::milliseconds delay) :
        delay(delay), timer(event, nullptr)
    {}

    void schedule() override
    {
        // 当定时器到达指定延迟时间后，会触发通过 setCallback 方法设置的回调函数
        timer.restartOnce(delay);
    }

    void setCallback(fu2::unique_function<void()>&& cb) override
    {
        timer.set_callback([cb = std::move(cb)](Timer&) mutable { cb(); });
    }

  private:
    std::chrono::milliseconds delay;
    Timer timer;
};

void termCb(sdeventplus::source::Signal& signal, const struct signalfd_siginfo*)
{
    lg2::notice("Received request to terminate, exiting");
    signal.get_event().exit(0);
}

int main()
{
    // 获取默认的事件循环对象，作为整个服务的事件驱动核心
    auto event = sdeventplus::Event::get_default();
    // 阻塞SIGTERM信号，防止在初始化过程中被中断
    stdplus::signal::block(SIGTERM);
    // 设置SIGTERM信号处理器，当收到终止信号时优雅退出
    // set_floating(true)表示信号源不需要手动管理生命周期
    // termCb函数在收到信号时会调用event.exit(0)退出事件循环
    // 这样设计保证了服务可以响应终止信号并完成必要的清理工作
    // 而非突然终止导致资源泄漏或配置丢失
    // 这是守护进程设计中的常见模式
    sdeventplus::source::Signal(event, SIGTERM, termCb).set_floating(true);

    // 创建DBus总线连接，使用Pinned智能指针管理生命周期
    // Pinned是一种特殊的智能指针，"确保对象地址不会改变"
    // 这对于需要稳定地址的回调和引用非常重要
    stdplus::Pinned bus = sdbusplus::bus::new_default();

    // 创建DBus对象管理器，管理指定路径下的所有DBus对象
    // DEFAULT_OBJPATH定义为"/xyz/openbmc_project/network"
    // 对象管理器的创建标志着DBus服务框架的初始化完成
    // 后续所有网络相关对象都将在这个路径下被创建和管理
    sdbusplus::server::manager_t objManager(bus, DEFAULT_OBJPATH);

    // 创建定时器执行器，用于延迟执行任务（如配置重载）
    // 延迟时间设置为3秒
    // TimerExecutor是一个自定义类，继承自DelayedExecutor接口
    // 它封装了sdeventplus的定时器功能，提供了schedule()和setCallback()方法
    // 这种设计模式允许Manager类通过接口而非具体实现来使用定时器功能
    // 提高了代码的可测试性和灵活性
    // 定时器执行器在配置更改后提供延迟执行机制
    // 这对于批量处理配置更改非常有用，可以避免频繁的系统调用
    stdplus::Pinned<TimerExecutor> reload(event, std::chrono::seconds(3));

    // 创建网络管理器的主对象，这是整个网络管理的核心组件
    // 参数包括：
    // - bus: DBus总线连接，用于与其他系统组件通信
    // - reload: 定时器执行器，用于延迟执行配置重载
    // - DEFAULT_OBJPATH: DBus对象路径前缀
    // - "/etc/systemd/network": 网络配置文件存储路径
    // Manager类负责管理所有网络接口、地址、路由和配置
    stdplus::Pinned<Manager> manager(bus, reload, DEFAULT_OBJPATH,
                                     "/etc/systemd/network");

    // 创建netlink服务器，用于与Linux内核网络子系统通信
    // 监听网络事件并通知manager处理
    // 这是连接用户空间和内核空间网络功能的桥梁
    netlink::Server svr(event, manager);

#ifdef SYNC_MAC_FROM_INVENTORY
    auto runtime = inventory::watch(bus, manager);
#endif

    bus.request_name(DEFAULT_BUSNAME);
    return sdeventplus::utility::loopWithBus(event, bus);
}

} // namespace phosphor::network

// 初始化流程分析
// 服务初始化流程遵循明确的依赖关系：

// 首先创建基础的事件循环和信号处理机制
// 然后建立DBus通信基础设施
// 创建辅助组件（定时器）
// 创建核心业务逻辑组件（Manager）
// 最后创建与系统集成的组件（Netlink服务器）
// 这种自底向上的初始化顺序确保了每个组件在创建时所需的依赖已经就绪。

int main(int /*argc*/, char** /*argv*/)
{
    try
    {
        return phosphor::network::main();
    }
    catch (const std::exception& e)
    {
        stdplus::print(stderr, "FAILED: {}", e.what());
        fflush(stderr);
        return 1;
    }
}
