#ifndef EXSIM_TESTS_COMMAND_GEN_HPP
#define EXSIM_TESTS_COMMAND_GEN_HPP
#include <sim/journal.hpp>
#include <random>
namespace exsim::testsupport {
struct CommandWeights { unsigned add=55,cancel=15,replace=18,mass=4,phase=8; };
inline sim::JournalSetup fuzz_setup(bool tiny=false,std::size_t instruments=3) {
    sim::JournalSetup s{{3,tiny?8u:128u},{}};
    for(auto name:{"AAA","BBB","CCC"}) {
        InstrumentConfig i;i.symbol=Symbol(name);i.min_price=990000;i.max_price=1010000;
        i.reference_price=1000000;i.max_order_qty=100;
        if(s.instruments.size()<instruments) s.instruments.push_back(i);
    }
    return s;
}
class CommandGenerator {
public:
    explicit CommandGenerator(std::uint64_t seed,CommandWeights weights={}):rng_(seed),weights_(weights) {}
    template<class Engine> Command next(const Engine& engine) {
        ++step_;
        std::vector<OrderInfo> orders;
        engine.for_each_order([&](const OrderInfo& o){orders.push_back(o);});
        OrderInfo live{};
        if(!orders.empty()) live=orders[pick(orders.size())];
        auto session=static_cast<SessionId>(1+pick(4));
        auto token=static_cast<ClOrdId>(1+pick(80));
        if(!orders.empty() && pick(5)!=0) {session=live.session;token=live.cl_ord_id;}
        const auto total=weights_.add+weights_.cancel+weights_.replace+weights_.mass+weights_.phase;
        if(!total) throw std::invalid_argument("zero generator weights");
        auto op=pick(total);
        if(op<weights_.add) {
            NewOrder n;n.ts=step_;n.instrument=static_cast<InstrumentId>(1+pick(engine.instrument_count()));
            n.session=static_cast<SessionId>(1+pick(4));n.owner=static_cast<OwnerId>(pick(3));
            n.cl_ord_id=++token_; if(pick(12)==0) {n.session=session;n.cl_ord_id=token;}
            n.side=static_cast<Side>(pick(2));n.type=static_cast<OrdType>(pick(5)==0?1:0);
            n.tif=static_cast<TimeInForce>(pick(4));n.stp=static_cast<StpMode>(pick(4));
            n.post_only=pick(12)==0;n.price=990000+static_cast<Price>(pick(5))*100;n.qty=static_cast<Quantity>(1+pick(100));
            if(pick(20)==0) switch(pick(8)) {
                case 0:n.instrument=4;break;case 1:n.session=0;break;case 2:n.cl_ord_id=0;break;
                case 3:n.side=static_cast<Side>(9);break;case 4:n.type=static_cast<OrdType>(9);break;
                case 5:n.tif=static_cast<TimeInForce>(9);break;case 6:n.stp=static_cast<StpMode>(9);break;
                case 7:n.qty=0;break;
            }
            if(pick(30)==0) n.price=-1;
            if(pick(40)==0) n.cl_ord_id=kMaxClOrdId+1;
            return n;
        }
        op-=weights_.add;
        if(op<weights_.cancel) return CancelOrder{step_,session,token,token_++,pick(2)?0u:static_cast<Quantity>(pick(101))};
        op-=weights_.cancel;
        if(op<weights_.replace) {
            ReplaceOrder r{step_,session,token,pick(3)?0:++token_,live.price?live.price:1000000,
                static_cast<Quantity>(pick(110)),pick(4)==0};
            if(pick(3)==0) r.price=990000+static_cast<Price>(pick(5))*100;
            if(pick(15)==0) r.cl_ord_id=kMaxClOrdId+1;
            if(pick(10)==0) r.cl_ord_id=1+pick(80);
            return r;
        }
        op-=weights_.replace;
        if(op<weights_.mass) return MassCancel{step_,session,static_cast<InstrumentId>(pick(4)),++token_,pick(2)!=0};
        return SetPhase{step_,static_cast<InstrumentId>(pick(5)),static_cast<Phase>(pick(20)==0?9:pick(5))};
    }
private:
    std::uint64_t pick(std::uint64_t n) {return rng_()%n;}
    std::mt19937_64 rng_;CommandWeights weights_;Timestamp step_=0;ClOrdId token_=100;
};
}
#endif
