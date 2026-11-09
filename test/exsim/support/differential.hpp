#ifndef EXSIM_TESTS_DIFFERENTIAL_HPP
#define EXSIM_TESTS_DIFFERENTIAL_HPP
#include "command_gen.hpp"
#include "format.hpp"
#include <test/exsim/reference/reference_engine.hpp>
#include <fstream>
namespace exsim::testsupport {
template<class Engine> void setup_engine(Engine& engine,const sim::JournalSetup& s) {
    for(const auto& i:s.instruments) if(!engine.add_instrument(i)) throw std::runtime_error("setup rejected");
}
template<class A,class B> std::string book_diff(const A& a,const B& b) {
    std::string x,y;
    auto snapshot=[](const auto& e) {
        std::string s="live="+std::to_string(e.live_order_count())+" instruments="+std::to_string(e.instrument_count())+"\n";
        for(std::size_t n=1;n<=e.instrument_count();++n) {
            auto id=static_cast<InstrumentId>(n);
            s+=std::string(e.instrument(id)->symbol.view())+" locate="+std::to_string(e.find_instrument(e.instrument(id)->symbol))+" phase="+std::string(to_string(e.phase(id)))+" bid="+format_optional_price(e.best_bid(id))+" ask="+format_optional_price(e.best_ask(id))+
              " last="+format_optional_price(e.last_trade_price(id))+" "+format_indicative(e.auction_indicative(id))+"\n";
            for(auto side:{Side::BUY,Side::SELL}) {
                std::vector<LevelInfo> levels(e.live_order_count()+1);
                auto count=e.depth(id,side,levels.data(),levels.size());
                for(std::size_t j=0;j<count;++j) {
                    s+=std::string(to_string(side))+" "+format_level(levels[j])+" fifo=";
                    std::vector<OrderId> ids(e.live_order_count()+1);
                    auto k=e.level_orders(id,side,levels[j].price,ids.data(),ids.size());
                    for(std::size_t l=0;l<k;++l) s+=std::to_string(ids[l])+",";
                    s+="\n";
                }
            }
        }
        e.for_each_order([&](const OrderInfo& o){
            s+=format_order(o)+"\n";
            auto id=e.find_order(o.order_id), token=e.find_order(o.session,o.cl_ord_id);
            if(!id || !token || !order_infos_equal(o,*id) || !order_infos_equal(o,*token)) s+="INVALID LOOKUP\n";
        });
        return s;
    };
    x=snapshot(a);y=snapshot(b);
    return x==y?std::string{}:"engine:\n"+x+"oracle:\n"+y;
}
template<class Engine> std::string replay_diff(const sim::JournalSetup& setup,const std::vector<Command>& commands,
                                              ref::Mutations mutations={}) {
    Engine a(setup.engine);ref::ReferenceEngine b(setup.engine,mutations);setup_engine(a,setup);setup_engine(b,setup);
    EventBuffer x(required_event_capacity(setup.engine)),y(required_event_capacity(setup.engine));
    for(std::size_t step=0;step<commands.size();++step) {
        x.clear();y.clear();
        auto ra=a.handle(commands[step],x),rb=b.handle(commands[step],y);
        bool equal=ra==rb && x.size()==y.size();
        for(std::size_t j=0;equal && j<x.size();++j) equal=events_equal(x[j],y[j]);
        auto books=book_diff(a,b);
        if(!equal || !books.empty()) return "step="+std::to_string(step)+" "+format_command(commands[step])+
            "\nengine events:\n"+format_events(x.begin(),x.end())+"oracle events:\n"+format_events(y.begin(),y.end())+books;
    }
    return {};
}
template<class Predicate> std::vector<Command> shrink(std::vector<Command> stream,Predicate fails) {
    for(std::size_t chunk=stream.size()/2;chunk;chunk/=2) {
        for(std::size_t i=0;i<stream.size();) {
            auto candidate=stream;candidate.erase(candidate.begin()+static_cast<std::ptrdiff_t>(i),
                candidate.begin()+static_cast<std::ptrdiff_t>(std::min(i+chunk,candidate.size())));
            if(fails(candidate)) {stream=std::move(candidate);i=0;} else i+=chunk;
        }
    }
    return stream;
}
inline void write_reproducer(const std::string& prefix,const sim::JournalSetup& setup,const std::vector<Command>& stream) {
    sim::JournalWriter journal(prefix+".journal",setup);
    std::ofstream text(prefix+".scn");
    text<<"config max_orders="<<setup.engine.max_orders<<" max_instruments="<<setup.engine.max_instruments<<"\n";
    for(const auto& i:setup.instruments) text<<"instrument "<<i.symbol.view()<<" tick="<<sim::format_price(i.tick_size)
        <<" lot="<<i.lot_size<<" max_qty="<<i.max_order_qty<<" min="<<sim::format_price(i.min_price)
        <<" max="<<sim::format_price(i.max_price)<<" ref="<<sim::format_price(i.reference_price)
        <<" phase="<<to_string(i.initial_phase)<<"\n";
    for(const auto& c:stream) {if(!journal.append(c)) throw std::runtime_error("journal append");text<<format_command(c)<<"\n";}
    if(!journal.flush()) throw std::runtime_error("journal flush");
}
template<class Engine> std::string fuzz(std::uint64_t seed,std::size_t steps,bool tiny=false,
                                       ref::Mutations mutations={},const std::string& artifact="/tmp/exsim-divergence") {
    auto setup=fuzz_setup(tiny,1+seed%3);Engine a(setup.engine);ref::ReferenceEngine b(setup.engine,mutations);
    setup_engine(a,setup);setup_engine(b,setup);CommandGenerator gen(seed);
    EventBuffer x(required_event_capacity(setup.engine)),y(required_event_capacity(setup.engine));
    std::vector<Command> stream;
    for(std::size_t step=0;step<steps;++step) {
        auto c=gen.next(b);stream.push_back(c);x.clear();y.clear();
        auto ra=a.handle(c,x),rb=b.handle(c,y);
        bool equal=ra==rb && x.size()==y.size();
        for(std::size_t j=0;equal && j<x.size();++j) equal=events_equal(x[j],y[j]);
        if(!equal || !book_diff(a,b).empty()) {
            auto minimal=shrink(stream,[&](const auto& v){return !replay_diff<Engine>(setup,v,mutations).empty();});
            write_reproducer(artifact,setup,minimal);
            return "seed="+std::to_string(seed)+" original step="+std::to_string(step)+
                " shrunk="+std::to_string(minimal.size())+" artifacts="+artifact+"\n"+replay_diff<Engine>(setup,minimal,mutations);
        }
    }
    return {};
}
}
#endif
