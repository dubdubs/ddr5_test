#include <map>
#include <memory>
#include <atomic>
#include <string>
#include <mutex>
#include <boost/interprocess/ipc/message_queue.hpp>

class BridgeDispatcher {
public:
    static BridgeDispatcher& getInstance();

    // 注册组件接口
    void register_component(DispatcherInterface* comp);

    // 发送消息
    bool send_msg(void* msg, uint32_t msg_size);

    // 静态工厂方法（注释中给出）
    // static BridgeDispatcher* CreateDispatcher(std::string ipc_name, uint8_t ipc_type);

    // 构造函数（公开，与单例模式冲突，请按实际需求调整）
    BridgeDispatcher(std::string ipc_name, uint8_t ipc_type);

private:
    BridgeDispatcher();  // 私有无参构造
    BridgeDispatcher(const BridgeDispatcher&) = delete;
    BridgeDispatcher& operator=(const BridgeDispatcher&) = delete;
    ~BridgeDispatcher();

    void receive_msg();
    void hello_handshake(uint32_t comp_id);

private:
    std::map<uint32_t, DispatcherInterface*> dispatch_table;
    std::unique_ptr<std::thread> recv_thread;
    std::atomic<bool> running_{true};
    uint8_t m_ipc_type;
    std::string m_ipc_name;
    ipc::channel* send_channel;
    ipc::channel* recv_channel;          // 修正：去掉空格，改为下划线
    ipc::channel* hello_channel;
    bool is_vp;
    std::mutex mtx;
    uint32_t trans_count{0};
    bool need_lock{true};

    // Boost 消息队列指针
    boost::interprocess::message_queue* mq_i{nullptr};
    boost::interprocess::message_queue* mq_o{nullptr};   // 修正：将 ] 改为 }
    boost::interprocess::message_queue* mq_hello{nullptr};

    // 读取缓冲区（请定义宏 SSLN_BOOSTMQ_MSGSZ）
    uint8_t read_buffer[SSLN_BOOSTMQ_MSGSZ];
};