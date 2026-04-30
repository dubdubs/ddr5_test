namespace ssln {
namespace hybrid {

class DispatcherInterface {
public:
    virtual ~DispatcherInterface() = default; 

    virtual bool HandleData(void* data) = 0;
    virtual uint32_t GetCompID() = 0;
};

}  // namespace hybrid
}  // namespace ssln