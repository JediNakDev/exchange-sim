#ifndef EXSIM_SIM_SCENARIO_HPP
#define EXSIM_SIM_SCENARIO_HPP
#include <sim/journal.hpp>
#include <sim/price_text.hpp>
#include <sim/format.hpp>
#include <sstream>
#include <map>
#include <memory>
namespace exsim::sim {
// Parsing and assertions are test/tool code; allocation is intentional.
class ScenarioNames {
public:
    std::map<std::string,std::uint64_t> instruments, sessions, owners, tokens;
    std::uint64_t value(const std::string& text,FieldKind kind) {
        if(kind==FieldKind::Price) { auto p=parse_price(text);if(!p) { throw std::runtime_error("invalid price"); }return to_raw(*p); }
        if(text=="true") return 1;
        if(text=="false") return 0;
        for(unsigned n=0;n<256;++n) if(enum_value_name(kind,n)==text) return n;
        if(!text.empty() && (text[0]=='-' || (text[0]>='0'&&text[0]<='9'))) {
            std::size_t used=0;auto n=std::stoull(text,&used,0);if(used!=text.size()) { throw std::runtime_error("invalid integer"); }return n;
        }
        auto* table=kind==FieldKind::Instrument?&instruments:kind==FieldKind::Session?&sessions:
                    kind==FieldKind::Owner?&owners:kind==FieldKind::Token?&tokens:nullptr;
        if(!table) throw std::runtime_error("unknown value: "+text);
        auto it=table->find(text);if(it!=table->end()) return it->second;
        if(kind==FieldKind::Instrument || kind==FieldKind::Session) throw std::runtime_error("undeclared name: "+text);
        return (*table)[text]=table->size()+1;
    }
};
inline Command parse_command(CommandType type,std::map<std::string,std::string> fields,ScenarioNames& names) {
    auto value=[&](const std::string& s,FieldKind k){return names.value(s,k);};
    switch(type) {
case CommandType::NewOrder: { NewOrder x;
if(auto it=fields.find("ts");it!=fields.end()) { x.ts=static_cast<decltype(x.ts)>(value(it->second,FieldKind::Timestamp)); fields.erase(it); }
if(auto it=fields.find("instrument");it!=fields.end()) { x.instrument=static_cast<decltype(x.instrument)>(value(it->second,FieldKind::Instrument)); fields.erase(it); }
if(auto it=fields.find("session");it!=fields.end()) { x.session=static_cast<decltype(x.session)>(value(it->second,FieldKind::Session)); fields.erase(it); }
if(auto it=fields.find("owner");it!=fields.end()) { x.owner=static_cast<decltype(x.owner)>(value(it->second,FieldKind::Owner)); fields.erase(it); }
if(auto it=fields.find("cl_ord_id");it!=fields.end()) { x.cl_ord_id=static_cast<decltype(x.cl_ord_id)>(value(it->second,FieldKind::Token)); fields.erase(it); }
if(auto it=fields.find("side");it!=fields.end()) { x.side=static_cast<decltype(x.side)>(value(it->second,FieldKind::Side)); fields.erase(it); }
if(auto it=fields.find("type");it!=fields.end()) { x.type=static_cast<decltype(x.type)>(value(it->second,FieldKind::OrdType)); fields.erase(it); }
if(auto it=fields.find("tif");it!=fields.end()) { x.tif=static_cast<decltype(x.tif)>(value(it->second,FieldKind::TimeInForce)); fields.erase(it); }
if(auto it=fields.find("stp");it!=fields.end()) { x.stp=static_cast<decltype(x.stp)>(value(it->second,FieldKind::StpMode)); fields.erase(it); }
if(auto it=fields.find("post_only");it!=fields.end()) { x.post_only=static_cast<decltype(x.post_only)>(value(it->second,FieldKind::Bool)); fields.erase(it); }
if(auto it=fields.find("price");it!=fields.end()) { x.price=static_cast<decltype(x.price)>(value(it->second,FieldKind::Price)); fields.erase(it); }
if(auto it=fields.find("qty");it!=fields.end()) { x.qty=static_cast<decltype(x.qty)>(value(it->second,FieldKind::Quantity)); fields.erase(it); }
if(!fields.empty()) { throw std::runtime_error("unknown command field: "+fields.begin()->first); } return Command{x}; }
case CommandType::Cancel: { CancelOrder x;
if(auto it=fields.find("ts");it!=fields.end()) { x.ts=static_cast<decltype(x.ts)>(value(it->second,FieldKind::Timestamp)); fields.erase(it); }
if(auto it=fields.find("session");it!=fields.end()) { x.session=static_cast<decltype(x.session)>(value(it->second,FieldKind::Session)); fields.erase(it); }
if(auto it=fields.find("orig_cl_ord_id");it!=fields.end()) { x.orig_cl_ord_id=static_cast<decltype(x.orig_cl_ord_id)>(value(it->second,FieldKind::Token)); fields.erase(it); }
if(auto it=fields.find("cl_ord_id");it!=fields.end()) { x.cl_ord_id=static_cast<decltype(x.cl_ord_id)>(value(it->second,FieldKind::Token)); fields.erase(it); }
if(auto it=fields.find("reduce_to_total_qty");it!=fields.end()) { x.reduce_to_total_qty=static_cast<decltype(x.reduce_to_total_qty)>(value(it->second,FieldKind::Quantity)); fields.erase(it); }
if(!fields.empty()) { throw std::runtime_error("unknown command field: "+fields.begin()->first); } return Command{x}; }
case CommandType::Replace: { ReplaceOrder x;
if(auto it=fields.find("ts");it!=fields.end()) { x.ts=static_cast<decltype(x.ts)>(value(it->second,FieldKind::Timestamp)); fields.erase(it); }
if(auto it=fields.find("session");it!=fields.end()) { x.session=static_cast<decltype(x.session)>(value(it->second,FieldKind::Session)); fields.erase(it); }
if(auto it=fields.find("orig_cl_ord_id");it!=fields.end()) { x.orig_cl_ord_id=static_cast<decltype(x.orig_cl_ord_id)>(value(it->second,FieldKind::Token)); fields.erase(it); }
if(auto it=fields.find("cl_ord_id");it!=fields.end()) { x.cl_ord_id=static_cast<decltype(x.cl_ord_id)>(value(it->second,FieldKind::Token)); fields.erase(it); }
if(auto it=fields.find("price");it!=fields.end()) { x.price=static_cast<decltype(x.price)>(value(it->second,FieldKind::Price)); fields.erase(it); }
if(auto it=fields.find("qty");it!=fields.end()) { x.qty=static_cast<decltype(x.qty)>(value(it->second,FieldKind::Quantity)); fields.erase(it); }
if(auto it=fields.find("force_priority_loss");it!=fields.end()) { x.force_priority_loss=static_cast<decltype(x.force_priority_loss)>(value(it->second,FieldKind::Bool)); fields.erase(it); }
if(!fields.empty()) { throw std::runtime_error("unknown command field: "+fields.begin()->first); } return Command{x}; }
case CommandType::MassCancel: { MassCancel x;
if(auto it=fields.find("ts");it!=fields.end()) { x.ts=static_cast<decltype(x.ts)>(value(it->second,FieldKind::Timestamp)); fields.erase(it); }
if(auto it=fields.find("session");it!=fields.end()) { x.session=static_cast<decltype(x.session)>(value(it->second,FieldKind::Session)); fields.erase(it); }
if(auto it=fields.find("instrument");it!=fields.end()) { x.instrument=static_cast<decltype(x.instrument)>(value(it->second,FieldKind::Instrument)); fields.erase(it); }
if(auto it=fields.find("cl_ord_id");it!=fields.end()) { x.cl_ord_id=static_cast<decltype(x.cl_ord_id)>(value(it->second,FieldKind::Token)); fields.erase(it); }
if(auto it=fields.find("cancel_on_disconnect");it!=fields.end()) { x.cancel_on_disconnect=static_cast<decltype(x.cancel_on_disconnect)>(value(it->second,FieldKind::Bool)); fields.erase(it); }
if(!fields.empty()) { throw std::runtime_error("unknown command field: "+fields.begin()->first); } return Command{x}; }
case CommandType::SetPhase: { SetPhase x;
if(auto it=fields.find("ts");it!=fields.end()) { x.ts=static_cast<decltype(x.ts)>(value(it->second,FieldKind::Timestamp)); fields.erase(it); }
if(auto it=fields.find("instrument");it!=fields.end()) { x.instrument=static_cast<decltype(x.instrument)>(value(it->second,FieldKind::Instrument)); fields.erase(it); }
if(auto it=fields.find("phase");it!=fields.end()) { x.phase=static_cast<decltype(x.phase)>(value(it->second,FieldKind::Phase)); fields.erase(it); }
if(!fields.empty()) { throw std::runtime_error("unknown command field: "+fields.begin()->first); } return Command{x}; }
default: throw std::runtime_error("unknown command");
    }
}
inline auto key_values(std::istringstream& line) {
    std::map<std::string,std::string> fields;std::string word;
    while(line>>word) { auto p=word.find('=');if(p==word.npos || !fields.emplace(word.substr(0,p),word.substr(p+1)).second)
        throw std::runtime_error("expected unique key=value"); }
    return fields;
}
template<class Engine> void run_scenario(std::istream& input) {
    ScenarioNames names;JournalSetup setup{{3,128},{}};
    std::unique_ptr<Engine> engine;std::unique_ptr<EventBuffer> out;
    auto start=[&](){if(!engine) {
        engine=std::make_unique<Engine>(setup.engine);
        for(const auto& i:setup.instruments) if(!engine->add_instrument(i)) throw std::runtime_error("invalid instrument");
        out=std::make_unique<EventBuffer>(required_event_capacity(setup.engine));
    }};
    std::size_t number=0,event_index=0;std::string text;
    while(std::getline(input,text)) { ++number;
        try {
            text=text.substr(0,text.find('#'));std::istringstream line(text);std::string op;
            if(!(line>>op)) continue;
            if(op=="config") {
                if(engine) throw std::runtime_error("late config");
                auto f=key_values(line);
                setup.engine.max_orders=std::stoull(f.at("max_orders"));
                if(f.count("max_instruments")) setup.engine.max_instruments=std::stoull(f.at("max_instruments"));
                continue;
            }
            if(op=="session") {
                std::string name,owner;line>>name>>owner;
                names.sessions[name]=names.sessions.size()+1;
                if(!owner.empty()) (void)names.value(owner,FieldKind::Owner);
                continue;
            }
            if(op=="instrument") {
                if(engine) throw std::runtime_error("late instrument");
                std::string symbol;line>>symbol;auto f=key_values(line);
                InstrumentConfig i;i.symbol=Symbol(symbol);
                for(const auto& [k,v]:f) {
                    if(k=="tick") i.tick_size=static_cast<Price>(names.value(v,FieldKind::Price));
                    else if(k=="lot") i.lot_size=static_cast<Quantity>(std::stoul(v));
                    else if(k=="max_qty") i.max_order_qty=static_cast<Quantity>(std::stoul(v));
                    else if(k=="min") i.min_price=static_cast<Price>(names.value(v,FieldKind::Price));
                    else if(k=="max") i.max_price=static_cast<Price>(names.value(v,FieldKind::Price));
                    else if(k=="ref") i.reference_price=static_cast<Price>(names.value(v,FieldKind::Price));
                    else if(k=="phase") i.initial_phase=static_cast<Phase>(names.value(v,FieldKind::Phase));
                    else throw std::runtime_error("unknown instrument field");
                }
                setup.instruments.push_back(i);names.instruments[symbol]=setup.instruments.size();continue;
            }
            start();
            if(op=="expect") {
                std::string what;line>>what;
                if(what=="no") { std::string events;line>>events;if(events!="events" || !out->empty()) { throw std::runtime_error("expected no events"); }continue; }
                if(what=="count") {std::size_t n;line>>n;if(n!=out->size()) { throw std::runtime_error("event count"); }continue;}
                if(what=="phase") {std::string symbol,p;line>>symbol>>p;
                    if(engine->phase(static_cast<InstrumentId>(names.value(symbol,FieldKind::Instrument)))!=static_cast<Phase>(names.value(p,FieldKind::Phase))) { throw std::runtime_error("phase mismatch"); }continue;}
                if(what=="depth") {
                    std::string symbol,side,level;line>>symbol>>side;
                    std::vector<LevelInfo> expected;
                    while(line>>level) {
                        auto a=level.find(':'),b=level.find(':',a+1);
                        if(a==level.npos || b==level.npos) throw std::runtime_error("depth uses price:qty:count");
                        expected.push_back({static_cast<Price>(names.value(level.substr(0,a),FieldKind::Price)),
                            std::stoull(level.substr(a+1,b-a-1)),static_cast<std::uint32_t>(std::stoul(level.substr(b+1)))});
                    }
                    std::vector<LevelInfo> actual(engine->live_order_count()+1);
                    auto count=engine->depth(static_cast<InstrumentId>(names.value(symbol,FieldKind::Instrument)),
                        side=="bid"?Side::BUY:Side::SELL,actual.data(),actual.size());
                    if(count!=expected.size()) throw std::runtime_error("depth count");
                    for(std::size_t j=0;j<count;++j) if(actual[j].price!=expected[j].price ||
                        actual[j].qty!=expected[j].qty || actual[j].order_count!=expected[j].order_count)
                        throw std::runtime_error("depth mismatch");
                    continue;
                }
                if(what=="book") {std::string symbol,side,price;std::uint64_t qty;std::uint32_t count;
                    if(!(line>>symbol>>side>>price>>qty>>count)) throw std::runtime_error("book requires symbol side price quantity count");
                    LevelInfo levels[4096];
                    auto n=engine->depth(static_cast<InstrumentId>(names.value(symbol,FieldKind::Instrument)),side=="bid"?Side::BUY:Side::SELL,levels,4096);
                    bool found=false;for(std::size_t j=0;j<n;++j) if(levels[j].price==static_cast<Price>(names.value(price,FieldKind::Price))) found=levels[j].qty==qty && levels[j].order_count==count;
                    if(!found) { throw std::runtime_error("book mismatch"); }continue;}
                auto fields=key_values(line);
                if(what=="order") {
                    auto session=static_cast<SessionId>(names.value(fields.at("session"),FieldKind::Session));
                    auto token=names.value(fields.at("cl_ord_id"),FieldKind::Token);auto order=engine->find_order(session,token);
                    if(!order) throw std::runtime_error("missing order");
                    visit_fields(*order,[&](const Field& f){auto it=fields.find(std::string(f.name));if(it!=fields.end()) {
                        if(f.raw!=names.value(it->second,f.kind)) { throw std::runtime_error("order field mismatch"); }fields.erase(it);}});
                } else {
                    if(event_index>=out->size()) throw std::runtime_error("missing event");
                    const auto& e=(*out)[event_index++];
                    std::string type(event_type_name(e.type));for(char& c:what) if(c>='A'&&c<='Z') c=static_cast<char>(c-'A'+'a');
                    if(what!=type) throw std::runtime_error("event type: "+format_event(e));
                    visit_fields(e,[&](const Field& f){auto it=fields.find(std::string(f.name));if(it!=fields.end()) {
                        if(f.raw!=names.value(it->second,f.kind)) { throw std::runtime_error("event field "+std::string(f.name)+": "+format_event(e)); }fields.erase(it);}});
                }
                if(!fields.empty()) { throw std::runtime_error("unknown assertion field"); }continue;
            }
            CommandType type=CommandType::NewOrder;bool found=false;
            for(unsigned n=0;n<=4;++n) if(to_string(static_cast<CommandType>(n))==op) {type=static_cast<CommandType>(n);found=true;}
            if(!found) throw std::runtime_error("unknown operation: "+op);
            auto command=parse_command(type,key_values(line),names);
            out->clear();event_index=0;
            if(engine->handle(command,*out)!=HandleResult::Applied) throw std::runtime_error("insufficient output");
        } catch(const std::exception& e) {throw std::runtime_error("line "+std::to_string(number)+": "+e.what());}
    }
}
}
#endif
